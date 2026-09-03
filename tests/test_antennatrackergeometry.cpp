#include <QtTest>

#include "ui/configuration/AntennaTrackerGeometry.h"

#include <cmath>
#include <limits>

namespace {

using Geometry = AntennaTrackerGeometry;
using Position = AntennaTrackerPosition;

const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

bool near(double actual, double expected, double tolerance = 1e-9)
{
    return std::fabs(actual - expected) <= tolerance;
}

#define VERIFY_NEAR(actual, expected, tolerance)                                               \
    QVERIFY2(near((actual), (expected), (tolerance)),                                          \
             qPrintable(QStringLiteral("%1 != %2 (tolerance %3)")                              \
                            .arg((actual), 0, 'g', 15)                                         \
                            .arg((expected), 0, 'g', 15)                                       \
                            .arg((tolerance))))

} // namespace

class AntennaTrackerGeometryTest final : public QObject
{
    Q_OBJECT

private slots:
    void positionsAndWraps();
    void cardinalDirections();
    void diagonalsAndLongitudeScale();
    void altitudeAndElevation();
    void zeroHorizontalDistance();
    void datelineCrossing();
    void polesAreSafe();
    void invalidInputIsRejected();
    void greatCircleReferences();
};

void AntennaTrackerGeometryTest::positionsAndWraps()
{
    QVERIFY(Position(0, 0).isValid());
    QVERIFY(Position(90, 180, 5000).isValid());
    QVERIFY(Position(-90, -180).isValid());
    QVERIFY(Position(0, 370).isValid()); // any finite longitude is normalised
    QVERIFY(!Position(90.0001, 0).isValid());
    QVERIFY(!Position(-91, 0).isValid());
    QVERIFY(!Position(kNaN, 0).isValid());
    QVERIFY(!Position(0, kNaN).isValid());
    QVERIFY(!Position(0, 0, kNaN).isValid());
    QVERIFY(!Position(0, kInf).isValid());
    QVERIFY(!Position(-kInf, 0).isValid());
    QVERIFY(!Position(0, 0, kInf).isValid());

    QCOMPARE(Position(1, 370, 3).normalised().longitude, 10.0);
    QCOMPARE(Position(1, -190).normalised().longitude, 170.0);
    QCOMPARE(Position(1, 180).normalised().longitude, -180.0);
    QCOMPARE(Position(1, -180).normalised().longitude, -180.0);
    QCOMPARE(Position(1, 540).normalised().longitude, -180.0);
    QCOMPARE(Position(1, 179.5).normalised().longitude, 179.5);
    QCOMPARE(Position(1, 370, 3).normalised().latitude, 1.0);
    QCOMPARE(Position(1, 370, 3).normalised().altitude, 3.0);
    QVERIFY(std::isnan(Position(1, kNaN).normalised().longitude));

    QCOMPARE(Geometry::Wrap180(181), -179.0);
    QCOMPARE(Geometry::Wrap180(-181), 179.0);
    QCOMPARE(Geometry::Wrap180(180), 180.0);
    QCOMPARE(Geometry::Wrap180(-180), -180.0);
    VERIFY_NEAR(Geometry::Wrap180(359.98), -0.02, 1e-9);
    QCOMPARE(Geometry::Wrap360(-1), 359.0);
    QCOMPARE(Geometry::Wrap360(360), 0.0);
    QCOMPARE(Geometry::Wrap360(0), 0.0);
    QCOMPARE(Geometry::Wrap360(359.5), 359.5);
    QCOMPARE(Geometry::NormaliseLongitude(0), 0.0);
    QCOMPARE(Geometry::NormaliseLongitude(-360), 0.0);
    QCOMPARE(Geometry::NormaliseLongitude(-181), 179.0);
    QCOMPARE(Geometry::RadiansToDegrees(Geometry::DegreesToRadians(123.456)), 123.456);
    QCOMPARE(Geometry::MetersPerDegree, 111319.5);
}

void AntennaTrackerGeometryTest::cardinalDirections()
{
    // Tracker at 45 N, 10 E, 100 m; vehicle 0.01 degrees away at the same altitude.
    const Position tracker(45.0, 10.0, 100.0);
    const double latitudeStep = 0.01 * Geometry::MetersPerDegree; // 1113.195 m exactly
    const double longitudeScale = std::cos(Geometry::DegreesToRadians(45.0));
    const double longitudeStep = 0.01 * Geometry::MetersPerDegree * longitudeScale;

    const Position north(45.01, 10.0, 100.0);
    VERIFY_NEAR(Geometry::DistToHome(tracker, north), latitudeStep, 1e-6);
    QCOMPARE(Geometry::AZToMAV(tracker, north), 0.0); // exactly due north
    QCOMPARE(Geometry::ELToMAV(tracker, north), 0.0);

    const Position east(45.0, 10.01, 100.0);
    VERIFY_NEAR(Geometry::DistToHome(tracker, east), longitudeStep, 1e-9);
    VERIFY_NEAR(Geometry::AZToMAV(tracker, east), 90.0, 1e-9);

    const Position south(44.99, 10.0, 100.0);
    VERIFY_NEAR(Geometry::DistToHome(tracker, south), latitudeStep, 1e-6);
    VERIFY_NEAR(Geometry::AZToMAV(tracker, south), 180.0, 1e-9);

    const Position west(45.0, 9.99, 100.0);
    VERIFY_NEAR(Geometry::DistToHome(tracker, west), longitudeStep, 1e-9);
    VERIFY_NEAR(Geometry::AZToMAV(tracker, west), 270.0, 1e-9);

    // PointAt agrees with the MP functions.
    const AntennaTrackerPointing pointing = Geometry::PointAt(tracker, west);
    QVERIFY(pointing.valid);
    QVERIFY(!pointing.coincident);
    VERIFY_NEAR(pointing.azimuth, 270.0, 1e-9);
    QCOMPARE(pointing.elevation, 0.0);
    VERIFY_NEAR(pointing.horizontalDistance, longitudeStep, 1e-9);
    QCOMPARE(pointing.altitudeDifference, 0.0);
    // Every azimuth stays inside [0, 360).
    VERIFY_NEAR(Geometry::AZToMAV(Position(0, 0), Position(0.001, -0.000001)), 359.94270424, 1e-6);
}

void AntennaTrackerGeometryTest::diagonalsAndLongitudeScale()
{
    // On the equator the longitude scale is 1: a NE diagonal is exactly 45.
    const Position origin(0.0, 0.0, 0.0);
    VERIFY_NEAR(Geometry::AZToMAV(origin, Position(0.01, 0.01)), 45.0, 1e-9);
    VERIFY_NEAR(Geometry::AZToMAV(origin, Position(-0.01, 0.01)), 135.0, 1e-9);
    VERIFY_NEAR(Geometry::AZToMAV(origin, Position(-0.01, -0.01)), 225.0, 1e-9);
    VERIFY_NEAR(Geometry::AZToMAV(origin, Position(0.01, -0.01)), 315.0, 1e-9);
    VERIFY_NEAR(Geometry::DistToHome(origin, Position(0.01, 0.01)),
                std::sqrt(2.0) * 0.01 * Geometry::MetersPerDegree, 1e-9);

    // At 60 N a degree of longitude is half a degree of latitude: the same
    // 0.01/0.01 offset points at atan(0.5) east of north.
    const Position tracker60(60.0, 0.0, 0.0);
    const double scale = std::cos(Geometry::DegreesToRadians(60.0));
    VERIFY_NEAR(scale, 0.5, 1e-12);
    VERIFY_NEAR(Geometry::AZToMAV(tracker60, Position(60.01, 0.01)),
                Geometry::RadiansToDegrees(std::atan2(0.01 * scale, 0.01)), 1e-9);
    VERIFY_NEAR(Geometry::AZToMAV(tracker60, Position(60.01, 0.01)), 26.565051177, 1e-6);
    VERIFY_NEAR(Geometry::DistToHome(tracker60, Position(60.01, 0.01)),
                0.01 * Geometry::MetersPerDegree * std::sqrt(1.0 + scale * scale), 1e-9);
    // The scale comes from the tracker latitude only (MP uses |TrackerLocation.Lat|).
    VERIFY_NEAR(Geometry::DistToHome(Position(-60.0, 0.0), Position(-60.0, 0.01)),
                0.01 * Geometry::MetersPerDegree * scale, 1e-9);
}

void AntennaTrackerGeometryTest::altitudeAndElevation()
{
    const Position tracker(45.0, 10.0, 100.0);
    const double distance = 0.01 * Geometry::MetersPerDegree; // 1113.195 m north
    const Position level(45.01, 10.0, 100.0);
    const Position above(45.01, 10.0, 100.0 + distance);
    const Position below(45.01, 10.0, 100.0 - distance);
    const Position higher(45.01, 10.0, 100.0 + distance * std::sqrt(3.0));

    QCOMPARE(Geometry::ELToMAV(tracker, level), 0.0);
    VERIFY_NEAR(Geometry::ELToMAV(tracker, above), 45.0, 1e-9);
    VERIFY_NEAR(Geometry::ELToMAV(tracker, below), -45.0, 1e-9);
    VERIFY_NEAR(Geometry::ELToMAV(tracker, higher), 60.0, 1e-9);
    VERIFY_NEAR(Geometry::ELToMAV(tracker, Position(45.01, 10.0, 100.0 + 1e6)), 89.936, 1e-3);

    const AntennaTrackerPointing pointing = Geometry::PointAt(tracker, above);
    QVERIFY(pointing.valid);
    VERIFY_NEAR(pointing.elevation, 45.0, 1e-9);
    VERIFY_NEAR(pointing.altitudeDifference, distance, 1e-9);
    QCOMPARE(pointing.azimuth, 0.0);
    // Only the altitude difference matters, not the datum offset.
    VERIFY_NEAR(Geometry::ELToMAV(Position(45.0, 10.0, -500.0), Position(45.01, 10.0, -500.0 + distance)),
                45.0, 1e-9);
    // Altitude never changes the azimuth or the horizontal distance.
    VERIFY_NEAR(Geometry::AZToMAV(tracker, above), Geometry::AZToMAV(tracker, level), 1e-12);
    QCOMPARE(Geometry::DistToHome(tracker, above), Geometry::DistToHome(tracker, level));
}

void AntennaTrackerGeometryTest::zeroHorizontalDistance()
{
    const Position tracker(45.0, 10.0, 100.0);
    const Position overhead(45.0, 10.0, 600.0);
    const Position underneath(45.0, 10.0, -400.0);
    const Position same(45.0, 10.0, 100.0);

    // MP CurrentState returns 0 for both angles when DistToHome is 0.
    QCOMPARE(Geometry::DistToHome(tracker, overhead), 0.0);
    QCOMPARE(Geometry::AZToMAV(tracker, overhead), 0.0);
    QCOMPARE(Geometry::ELToMAV(tracker, overhead), 0.0);
    QCOMPARE(Geometry::ELToMAV(tracker, underneath), 0.0);

    // PointAt is honest about the vertical: +90 above, -90 below, 0 at the tracker.
    AntennaTrackerPointing pointing = Geometry::PointAt(tracker, overhead);
    QVERIFY(pointing.valid);
    QVERIFY(pointing.coincident);
    QCOMPARE(pointing.azimuth, 0.0);
    QCOMPARE(pointing.elevation, 90.0);
    QCOMPARE(pointing.horizontalDistance, 0.0);
    QCOMPARE(pointing.altitudeDifference, 500.0);
    pointing = Geometry::PointAt(tracker, underneath);
    QVERIFY(pointing.coincident);
    QCOMPARE(pointing.elevation, -90.0);
    QCOMPARE(pointing.altitudeDifference, -500.0);
    pointing = Geometry::PointAt(tracker, same);
    QVERIFY(pointing.valid);
    QVERIFY(pointing.coincident);
    QCOMPARE(pointing.azimuth, 0.0);
    QCOMPARE(pointing.elevation, 0.0);
    QCOMPARE(pointing.altitudeDifference, 0.0);
    // The same spot written with a wrapped longitude is still coincident.
    QVERIFY(Geometry::PointAt(tracker, Position(45.0, 370.0, 100.0)).coincident);
}

void AntennaTrackerGeometryTest::datelineCrossing()
{
    // The vehicle is 0.02 degrees east of the tracker across the dateline.
    const Position tracker(0.0, 179.99, 0.0);
    const Position east(0.0, -179.99, 0.0);
    const double distance = 0.02 * Geometry::MetersPerDegree;
    VERIFY_NEAR(Geometry::DistToHome(tracker, east), distance, 1e-6);
    VERIFY_NEAR(Geometry::AZToMAV(tracker, east), 90.0, 1e-9);
    // ... and looking back it is 0.02 degrees west.
    VERIFY_NEAR(Geometry::DistToHome(east, tracker), distance, 1e-6);
    VERIFY_NEAR(Geometry::AZToMAV(east, tracker), 270.0, 1e-9);
    // Un-normalised longitudes give the same answer.
    VERIFY_NEAR(Geometry::AZToMAV(tracker, Position(0.0, 180.01)), 90.0, 1e-9);
    VERIFY_NEAR(Geometry::DistToHome(tracker, Position(0.0, 180.01)), distance, 1e-6);
    VERIFY_NEAR(Geometry::AZToMAV(Position(0.0, -179.99), Position(0.0, -180.01)), 270.0, 1e-9);
    // A diagonal across the dateline keeps its quadrant.
    VERIFY_NEAR(Geometry::AZToMAV(Position(10.0, 179.99), Position(10.01, -179.99 + 0.0)),
                Geometry::RadiansToDegrees(
                    std::atan2(0.02 * std::cos(Geometry::DegreesToRadians(10.0)), 0.01)),
                1e-9);
    // Exactly half way round the world is a wrap boundary, not a huge distance.
    VERIFY_NEAR(Geometry::DistToHome(Position(0.0, 0.0), Position(0.0, 180.0)),
                180.0 * Geometry::MetersPerDegree, 1e-6);
    const AntennaTrackerPointing pointing = Geometry::PointAt(tracker, Position(0.0, -179.99, 1000.0));
    QVERIFY(pointing.valid && !pointing.coincident);
    VERIFY_NEAR(pointing.azimuth, 90.0, 1e-9);
    VERIFY_NEAR(pointing.elevation, Geometry::RadiansToDegrees(std::atan(1000.0 / distance)), 1e-9);
}

void AntennaTrackerGeometryTest::polesAreSafe()
{
    // A tracker on the north pole sees everything to the south; the longitude
    // difference contributes nothing at cos(90) and nothing overflows.
    const Position northPole(90.0, 0.0, 0.0);
    const Position nearby(89.9, 45.0, 0.0);
    VERIFY_NEAR(Geometry::DistToHome(northPole, nearby), 0.1 * Geometry::MetersPerDegree, 1e-6);
    VERIFY_NEAR(Geometry::AZToMAV(northPole, nearby), 180.0, 1e-6);
    QVERIFY(std::isfinite(Geometry::AZToMAV(northPole, Position(-90.0, 180.0))));
    VERIFY_NEAR(Geometry::AZToMAV(northPole, Position(-90.0, 180.0)), 180.0, 1e-6);
    VERIFY_NEAR(Geometry::DistToHome(northPole, Position(-90.0, 180.0)),
                180.0 * Geometry::MetersPerDegree, 1e-6);

    const Position southPole(-90.0, 30.0, 0.0);
    QCOMPARE(Geometry::AZToMAV(southPole, Position(-89.9, -120.0)), 0.0);
    VERIFY_NEAR(Geometry::DistToHome(southPole, Position(-89.9, -120.0)),
                0.1 * Geometry::MetersPerDegree, 1e-6);

    // A vehicle over a pole is an ordinary target for a tracker below it.
    const Position tracker(89.0, 0.0, 0.0);
    const AntennaTrackerPointing pointing = Geometry::PointAt(tracker, Position(90.0, 100.0, 500.0));
    QVERIFY(pointing.valid);
    QVERIFY(!pointing.coincident);
    QVERIFY(std::isfinite(pointing.azimuth));
    QVERIFY(pointing.azimuth >= 0.0 && pointing.azimuth < 360.0);
    const double east = 100.0 * Geometry::MetersPerDegree * std::cos(Geometry::DegreesToRadians(89.0));
    const double north = 1.0 * Geometry::MetersPerDegree;
    VERIFY_NEAR(pointing.azimuth, Geometry::RadiansToDegrees(std::atan2(east, north)), 1e-9);
    VERIFY_NEAR(pointing.horizontalDistance, std::sqrt(east * east + north * north), 1e-6);
    // Two points on the same pole are coincident whatever their longitudes.
    QVERIFY(Geometry::PointAt(northPole, Position(90.0, 123.0, 10.0)).coincident);
    QCOMPARE(Geometry::PointAt(northPole, Position(90.0, 123.0, 10.0)).elevation, 90.0);
}

void AntennaTrackerGeometryTest::invalidInputIsRejected()
{
    const Position good(45.0, 10.0, 100.0);
    const Position badLatitude(91.0, 10.0, 100.0);
    const Position nanLongitude(45.0, kNaN, 100.0);
    const Position infAltitude(45.0, 10.0, kInf);

    for (const Position &bad : {badLatitude, nanLongitude, infAltitude}) {
        QCOMPARE(Geometry::DistToHome(good, bad), 0.0);
        QCOMPARE(Geometry::AZToMAV(good, bad), 0.0);
        QCOMPARE(Geometry::ELToMAV(good, bad), 0.0);
        QCOMPARE(Geometry::DistToHome(bad, good), 0.0);
        QCOMPARE(Geometry::AZToMAV(bad, good), 0.0);
        QCOMPARE(Geometry::ELToMAV(bad, good), 0.0);
        QCOMPARE(Geometry::GetBearing(good, bad), 0.0);
        QCOMPARE(Geometry::GetDistance(bad, good), 0.0);
        const AntennaTrackerPointing pointing = Geometry::PointAt(good, bad);
        QVERIFY(!pointing.valid);
        QVERIFY(!pointing.coincident);
        QCOMPARE(pointing.azimuth, 0.0);
        QCOMPARE(pointing.elevation, 0.0);
        QCOMPARE(pointing.horizontalDistance, 0.0);
        QCOMPARE(pointing.altitudeDifference, 0.0);
        QVERIFY(!Geometry::PointAt(bad, good).valid);
    }
    QVERIFY(!Geometry::PointAt(badLatitude, nanLongitude).valid);
    // The MP "no fix" sentinel (0, 0) is an ordinary, valid position here.
    QVERIFY(Geometry::PointAt(Position(0.0, 0.0), Position(0.01, 0.0)).valid);
    QCOMPARE(Geometry::AZToMAV(Position(0.0, 0.0), Position(0.01, 0.0)), 0.0);
}

void AntennaTrackerGeometryTest::greatCircleReferences()
{
    const Position origin(0.0, 0.0, 0.0);
    VERIFY_NEAR(Geometry::GetBearing(origin, Position(0.0, 1.0)), 90.0, 1e-9);
    QCOMPARE(Geometry::GetBearing(origin, Position(1.0, 0.0)), 0.0);
    VERIFY_NEAR(Geometry::GetBearing(origin, Position(-1.0, 0.0)), 180.0, 1e-9);
    VERIFY_NEAR(Geometry::GetBearing(origin, Position(0.0, -1.0)), 270.0, 1e-9);
    VERIFY_NEAR(Geometry::GetBearing(origin, Position(1.0, 1.0)), 44.995636455, 1e-6);
    // One degree along the equator is R * pi / 180 on the 6371 km sphere.
    VERIFY_NEAR(Geometry::GetDistance(origin, Position(0.0, 1.0)),
                Geometry::EarthRadiusMeters * Geometry::DegreesToRadians(1.0), 1e-6);
    VERIFY_NEAR(Geometry::GetDistance(origin, Position(0.0, 1.0)), 111194.926645, 1e-3);
    QCOMPARE(Geometry::GetDistance(origin, origin), 0.0);
    // Antipodes: the haversine fraction reaches exactly 1 (or a rounding hair
    // past it); the distance is half the circumference, never NaN.
    const double halfCircumference = Geometry::EarthRadiusMeters * Geometry::DegreesToRadians(180.0);
    VERIFY_NEAR(Geometry::GetDistance(origin, Position(0.0, 180.0)), halfCircumference, 1e-3);
    VERIFY_NEAR(Geometry::GetDistance(Position(90.0, 0.0), Position(-90.0, 0.0)), halfCircumference, 1e-3);
    VERIFY_NEAR(Geometry::GetDistance(Position(45.0, 10.0), Position(-45.0, -170.0)), halfCircumference, 1e-3);
    for (const Position &nearAntipode : {Position(0.0000001, 179.9999999), Position(-0.0000002, -179.9999998),
                                         Position(0.0, 179.99999999999997), Position(1e-12, 180.0)}) {
        const double distance = Geometry::GetDistance(origin, nearAntipode);
        QVERIFY2(std::isfinite(distance), qPrintable(QString::number(distance)));
        QVERIFY(distance > halfCircumference - 1.0 && distance <= halfCircumference + 1e-6);
        QVERIFY(std::isfinite(Geometry::GetBearing(origin, nearAntipode)));
    }
    QVERIFY(std::isfinite(Geometry::GetDistance(Position(90.0, 0.0), Position(-90.0, 180.0))));
    VERIFY_NEAR(Geometry::GetDistance(Position(90.0, 0.0), Position(-90.0, 180.0)), halfCircumference, 1e-3);
    VERIFY_NEAR(Geometry::GetDistance(Position(0.0, 179.99), Position(0.0, -179.99)),
                Geometry::EarthRadiusMeters * Geometry::DegreesToRadians(0.02), 1e-6);

    // Over a few kilometres the MP flat-earth model and the great circle
    // agree closely; the tracker only needs that regime.
    const Position tracker(45.0, 10.0, 0.0);
    const Position vehicle(45.02, 10.03, 0.0);
    const double flat = Geometry::DistToHome(tracker, vehicle);
    const double sphere = Geometry::GetDistance(tracker, vehicle);
    QVERIFY(flat > 3000.0 && sphere > 3000.0);
    QVERIFY(std::fabs(flat - sphere) / sphere < 0.002);
    QVERIFY(std::fabs(Geometry::AZToMAV(tracker, vehicle) - Geometry::GetBearing(tracker, vehicle)) < 0.05);
}

QTEST_APPLESS_MAIN(AntennaTrackerGeometryTest)
#include "test_antennatrackergeometry.moc"
