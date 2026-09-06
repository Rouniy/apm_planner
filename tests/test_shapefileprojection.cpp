#include <QtTest>
#include "services/ShapefileProjection.h"
#include "services/NativeGdalLibrary.h"

#include <QLibrary>
#include <QLocale>
#include <cmath>
#include <limits>
#include <memory>

namespace {
using namespace Shapefile;
QString geographic() {
    return QStringLiteral("GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"Degree\",0.017453292519943295]]");
}
QString projected(const QString &name, const QString &method, const QString &parameters,
                  const QString &units = QStringLiteral("UNIT[\"Meter\",1]")) {
    return QStringLiteral("PROJCS[\"%1\",%2,PROJECTION[\"%3\"],%4,%5]")
        .arg(name, geographic(), method, parameters, units);
}
QString utm(int zone, bool south) {
    return projected(QStringLiteral("WGS_1984_UTM_Zone_%1%2").arg(zone).arg(south ? 'S' : 'N'),
        QStringLiteral("Transverse_Mercator"),
        QStringLiteral("PARAMETER[\"False_Easting\",500000],PARAMETER[\"False_Northing\",%1],PARAMETER[\"Central_Meridian\",%2],PARAMETER[\"Scale_Factor\",0.9996],PARAMETER[\"Latitude_Of_Origin\",0]")
            .arg(south ? 10000000 : 0).arg(zone * 6 - 183));
}
QVector<Feature> points(std::initializer_list<Coordinate> values) {
    Feature f; for (const auto &p : values) f.points.append(p); return {f};
}
bool runtimeMissing(const ProjectionResult &r) {
    return r.error.startsWith("Native GDAL runtime") || r.error.startsWith("Native PROJ 9.2");
}
}

class ShapefileProjectionTest : public QObject {
    Q_OBJECT
private slots:
    void rawWgs84FilteringAltitudeAndBoundaries() {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        auto source = points({{15,47,12}, {-180,-90,nan}, {180,90,9}, {181,0,0}, {0,-91,0}, {nan,0,0}});
        source.append(Feature{}); source.append(points({{1,2,3}}).first());
        const auto result = ShapefileProjection::transform({}, source);
        QVERIFY2(result.success, qPrintable(result.error)); QVERIFY(result.projectionName.isEmpty());
        QCOMPARE(result.features.size(), 3); QCOMPARE(result.features.at(0).points.size(), 3);
        QCOMPARE(result.features.at(0).points.at(1).z, 0.0); QVERIFY(result.features.at(1).points.isEmpty());
        QCOMPARE(result.features.at(2).points.first().x, 1.0); QCOMPARE(result.discardedPoints, qint64(3));
        QVERIFY(result.warnings.join(' ').contains("actual datum is not verified"));
    }

    void projectedGolden_data() {
        QTest::addColumn<QString>("wkt"); QTest::addColumn<double>("x"); QTest::addColumn<double>("y");
        QTest::addColumn<double>("longitude"); QTest::addColumn<double>("latitude");
        QTest::newRow("UTM33N") << utm(33, false) << 515176.7201 << 5216296.2492 << 15.2 << 47.1;
        QTest::newRow("UTM55S") << utm(55, true) << 696706.8530 << 6084555.7052 << 149.165085 << -35.362938;
        QTest::newRow("WebMercator") << projected("WGS_1984_Web_Mercator_Auxiliary_Sphere", "Mercator_Auxiliary_Sphere",
            "PARAMETER[\"False_Easting\",0],PARAMETER[\"False_Northing\",0],PARAMETER[\"Central_Meridian\",0],PARAMETER[\"Standard_Parallel_1\",0],PARAMETER[\"Auxiliary_Sphere_Type\",0]")
            << 1692056.2601 << 5958411.9200 << 15.2 << 47.1;
        // Independent geometric identities: conic projections map their false
        // origin to the configured longitude/latitude, not WGS84 (0,0).
        QTest::newRow("LambertConformalConic") << projected("Custom_WGS84_Lambert", "Lambert_Conformal_Conic",
            "PARAMETER[\"False_Easting\",600000],PARAMETER[\"False_Northing\",200000],PARAMETER[\"Central_Meridian\",10],PARAMETER[\"Standard_Parallel_1\",45],PARAMETER[\"Standard_Parallel_2\",55],PARAMETER[\"Latitude_Of_Origin\",52]")
            << 600000.0 << 200000.0 << 10.0 << 52.0;
        QTest::newRow("AlbersEqualArea") << projected("Custom_WGS84_Albers", "Albers",
            "PARAMETER[\"False_Easting\",0],PARAMETER[\"False_Northing\",0],PARAMETER[\"Central_Meridian\",-96],PARAMETER[\"Standard_Parallel_1\",29.5],PARAMETER[\"Standard_Parallel_2\",45.5],PARAMETER[\"Latitude_Of_Origin\",23]")
            << 0.0 << 0.0 << -96.0 << 23.0;
    }
    void projectedGolden() {
        QFETCH(QString, wkt); QFETCH(double, x); QFETCH(double, y);
        QFETCH(double, longitude); QFETCH(double, latitude);
        const QLocale previous = QLocale(); QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
        const auto result = ShapefileProjection::transform(wkt, points({{x,y,123}}));
        QLocale::setDefault(previous);
        if (runtimeMissing(result)) QSKIP(qPrintable(result.error));
        QVERIFY2(result.success, qPrintable(result.error)); QVERIFY(!result.projectionName.isEmpty());
        QCOMPARE(result.features.size(), 1); QCOMPARE(result.features.first().points.size(), 1);
        const auto p = result.features.first().points.first();
        QVERIFY2(std::abs(p.x-longitude) < 1e-6, qPrintable(QString::number(p.x, 'g', 17)));
        QVERIFY2(std::abs(p.y-latitude) < 1e-6, qPrintable(QString::number(p.y, 'g', 17)));
        QVERIFY(std::abs(p.z-123) < 1e-8);
    }

    void geographicUnitsAndExplicitDatumShift() {
        QString grads = geographic();
        grads.replace("\"Degree\",0.017453292519943295", "\"grad\",0.015707963267948967");
        const auto angular = ShapefileProjection::transform(grads, points({{100,50,0}}));
        if (runtimeMissing(angular)) QSKIP(qPrintable(angular.error));
        QVERIFY2(angular.success, qPrintable(angular.error)); QCOMPARE(angular.features.first().points.size(), 1);
        QVERIFY(std::abs(angular.features.first().points.first().x - 90) < 1e-8);
        QVERIFY(std::abs(angular.features.first().points.first().y - 45) < 1e-8);
        const QString shifted = QStringLiteral("GEOGCS[\"Explicit_test_datum\",DATUM[\"Custom_shift\",SPHEROID[\"WGS84\",6378137,298.257223563],TOWGS84[100,0,0,0,0,0,0]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.017453292519943295]]");
        const auto result = ShapefileProjection::transform(shifted, points({{90,0,0}}));
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.features.first().points.size(), 1);
        const auto p = result.features.first().points.first();
        // Geocentric X+100 at the equator: longitude=atan2(a,100).
        const double expected = std::atan2(6378137.0, 100.0) * 180 / std::acos(-1.0);
        QVERIFY(std::abs(p.x - expected) < 1e-8); QVERIFY(std::abs(p.y) < 1e-10);
        QVERIFY(std::abs(p.x-90) > 0.0008);
    }

    void unknownDatumCannotBecomeSilentIdentity() {
        QString unknown = geographic(); unknown.replace("GCS_WGS_1984", "Unregistered_source");
        unknown.replace("D_WGS_1984", "Unknown_datum_no_transform");
        unknown.replace("WGS_1984\",6378137,298.257223563", "Unknown_ellipsoid\",6370000,300");
        const auto result = ShapefileProjection::transform(unknown, points({{10,20,0}}));
        if (runtimeMissing(result)) QSKIP(qPrintable(result.error));
        QVERIFY(!result.success); QVERIFY(!result.error.isEmpty()); QVERIFY(result.features.isEmpty());
    }

    void projectedInvalidCoordinatesAndAmbiguousPrimeMeridian() {
        const auto filtered = ShapefileProjection::transform(geographic(), points({{15,47,0},{0,91,0}}));
        if (runtimeMissing(filtered)) QSKIP(qPrintable(filtered.error));
        QVERIFY2(filtered.success, qPrintable(filtered.error));
        QCOMPARE(filtered.discardedPoints, qint64(1)); QCOMPARE(filtered.features.first().points.size(), 1);
        QString paris = geographic(); paris.replace("\"Greenwich\",0", "\"Paris\",2.337229166667");
        // A different prime meridian changes datum equivalence in PROJ.
        // Merely relabelling canonical WGS84 does not establish a datum
        // transformation: its inferred longitude rotation is ballpark.
        const auto ambiguous = ShapefileProjection::transform(paris, points({{0,45,0}}));
        QVERIFY(!ambiguous.success); QVERIFY(!ambiguous.error.isEmpty());
        QVERIFY(ambiguous.features.isEmpty());
    }

    void explicitBoundPrimeMeridians_data() {
        QTest::addColumn<QString>("meridian"); QTest::addColumn<double>("longitude");
        QTest::newRow("Paris") << QStringLiteral("Paris") << 2.337229166667;
        QTest::newRow("Rome") << QStringLiteral("Rome") << 12.452333333333;
        QTest::newRow("Lisbon") << QStringLiteral("Lisbon") << -9.131906111111;
    }
    void explicitBoundPrimeMeridians() {
        QFETCH(QString, meridian); QFETCH(double, longitude);
        // This synthetic datum explicitly declares geocentric equivalence
        // through TOWGS84. Its strict pipeline rotates the prime meridian
        // without enabling ballpark fallback or guessing datum equivalence.
        // Preserve original WKT1 semantics: an intermediate WKT2 BOUNDCRS
        // round trip is not equivalent for these transformation-source CRSs.
        const QString bound = QStringLiteral("GEOGCS[\"Explicit_%1_frame\",DATUM[\"Custom_%1_frame\",SPHEROID[\"WGS84\",6378137,298.257223563],TOWGS84[0,0,0,0,0,0,0]],PRIMEM[\"%1\",%2],UNIT[\"degree\",0.017453292519943295]]")
            .arg(meridian, QString::number(longitude, 'g', 17));
        const auto result = ShapefileProjection::transform(bound, points({{0,45,0}}));
        if (runtimeMissing(result)) QSKIP(qPrintable(result.error));
        QVERIFY2(result.success, qPrintable(result.error)); QCOMPARE(result.features.first().points.size(), 1);
        QVERIFY(std::abs(result.features.first().points.first().x - longitude) < 1e-8);
        QVERIFY(std::abs(result.features.first().points.first().y - 45) < 1e-8);
    }

    void missingExplicitGridNeverBecomesSuccessfulIdentity() {
        const QString wkt = QStringLiteral("GEOGCS[\"NAD27_missing_grid\",DATUM[\"North_American_Datum_1927\",SPHEROID[\"Clarke 1866\",6378206.4,294.978698213898],EXTENSION[\"PROJ4_GRIDS\",\"apm_nonexistent_grid_7f39c810.gsb\"]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.017453292519943295]]");
        const auto result = ShapefileProjection::transform(wkt, points({{-100,40,0}}));
        if (runtimeMissing(result)) QSKIP(qPrintable(result.error));
        QVERIFY(!result.success); QVERIFY(!result.error.isEmpty()); QVERIFY(result.features.isEmpty());
    }

    void badWktAndLimits_data() {
        QTest::addColumn<QString>("wkt");
        QTest::newRow("invalid") << QStringLiteral("GEOGCS[broken");
        QTest::newRow("url-not-wkt") << QStringLiteral("https://example.invalid/coordinate-system");
        QTest::newRow("authority-not-wkt") << QStringLiteral("EPSG:4326");
        QTest::newRow("trailing-garbage") << (geographic() + "junk");
        QTest::newRow("embedded-nul") << (geographic() + QChar('\0') + "ignored");
        QTest::newRow("oversize") << QString(int(MaximumPrjBytes)+1, ' ');
        QTest::newRow("utf8-oversize") << QString(24000, QChar(0x4e2d));
    }
    void badWktAndLimits() {
        QFETCH(QString, wkt);
        const auto result = ShapefileProjection::transform(wkt, points({{1,2,0}}));
        QVERIFY(!result.success); QVERIFY(!result.error.isEmpty()); QVERIFY(result.features.isEmpty());
    }

    void emptyBomFeatureAndPointLimits() {
        const auto empty = ShapefileProjection::transform(QString(QChar(0xfeff)) + " \n", {});
        QVERIFY(empty.success); QVERIFY(empty.features.isEmpty());
        QVector<Feature> many(MaximumFeatures + 1);
        QVERIFY(!ShapefileProjection::transform({}, many).success);
        Feature large; large.points.resize(MaximumPoints + 1);
        QVERIFY(!ShapefileProjection::transform({}, {large}).success);
    }

    void cancellationChunksAndPinnedInput() {
        auto source = points({{1,2,3}}); source.first().points.fill({1,2,3}, 5000);
        QString wkt;
        bool stop = false; QVector<qint64> completed;
        const auto result = ShapefileProjection::transform(wkt, source, [&] { return stop; },
            [&](qint64 count, qint64 total, const QString &) {
                QCOMPARE(total, qint64(5000)); completed.append(count);
                if (count == 2048) { stop = true; source.clear(); wkt = "invalid mutated caller"; }
            });
        QVERIFY(result.cancelled); QVERIFY(!result.success); QVERIFY(result.features.isEmpty());
        QCOMPARE(completed, QVector<qint64>({0,2048}));
        const auto early = ShapefileProjection::transform(geographic(), points({{0,0,0}}), [] { return true; });
        QVERIFY(early.cancelled); QVERIFY(early.features.isEmpty());
        const auto late = ShapefileProjection::transform({}, points({{1,2,3}}), [&] { return stop; }, {});
        QVERIFY(late.cancelled);
    }

    void successfulProgressAndFinalCancellation() {
        auto source = points({{1,2,3}}); source.first().points.fill({1,2,3}, 5000);
        QVector<qint64> completed;
        const auto ok = ShapefileProjection::transform({}, source, {}, [&](qint64 n,qint64,const QString &) { completed.append(n); });
        QVERIFY(ok.success); QCOMPARE(completed, QVector<qint64>({0,2048,4096,5000}));
        QCOMPARE(ok.features.first().points.size(), 5000);
        bool stop = false;
        const auto cancelled = ShapefileProjection::transform({}, source, [&] { return stop; },
            [&](qint64 n,qint64 total,const QString &) { if(n == total) stop = true; });
        QVERIFY(cancelled.cancelled); QVERIFY(!cancelled.success); QVERIFY(cancelled.features.isEmpty());
    }

    void unrelatedProjContextNetworkRemainsUntouched() {
        std::unique_ptr<QLibrary> library;
        for (const auto &candidate : nativeProjLibraryCandidates()) {
            auto attempt = std::make_unique<QLibrary>(candidate);
            attempt->setLoadHints(QLibrary::PreventUnloadHint);
            if (attempt->load()) { library = std::move(attempt); break; }
        }
        if (!library) QSKIP("Optional PROJ runtime unavailable.");
        const auto create = reinterpret_cast<void *(*)()>(library->resolve("proj_context_create"));
        const auto destroy = reinterpret_cast<void *(*)(void *)>(library->resolve("proj_context_destroy"));
        const auto enable = reinterpret_cast<int (*)(void *,int)>(library->resolve("proj_context_set_enable_network"));
        const auto enabled = reinterpret_cast<int (*)(void *)>(library->resolve("proj_context_is_network_enabled"));
        if (!create || !destroy || !enable || !enabled) QSKIP("PROJ networking context API unavailable.");
        void *context = create(); QVERIFY(context);
        enable(context, 1);
        bool isolation = enabled(context) == 1;
        const auto result = ShapefileProjection::transform(utm(33,false), points({{500000,0,0}}), {},
            [&](qint64,qint64,const QString &) { isolation = isolation && enabled(context) == 1; });
        isolation = isolation && enabled(context) == 1; destroy(context);
        if (runtimeMissing(result)) QSKIP(qPrintable(result.error));
        QVERIFY2(result.success, qPrintable(result.error)); QVERIFY(isolation);
    }
};
QTEST_GUILESS_MAIN(ShapefileProjectionTest)
#include "test_shapefileprojection.moc"
