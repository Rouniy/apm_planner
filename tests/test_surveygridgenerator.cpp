#include "ui/flightplanner/SurveyGridGenerator.h"

#include <QtTest>

#include <cmath>
#include <limits>

namespace {

constexpr double kEarthRadiusMeters = 6378137.0;
constexpr double kRadiansToDegrees = 57.295779513082320877;

SurveyGridCoordinate atMeters(double north, double east,
                              double latitude = 47.0,
                              double longitude = 8.0)
{
    SurveyGridCoordinate coordinate;
    coordinate.latitude = latitude
        + north * kRadiansToDegrees / kEarthRadiusMeters;
    coordinate.longitude = longitude
        + east * kRadiansToDegrees
            / (kEarthRadiusMeters * std::cos(latitude / kRadiansToDegrees));
    return coordinate;
}

QVector<SurveyGridCoordinate> rectangle(double width = 100.0,
                                        double height = 100.0)
{
    return {
        atMeters(0.0, 0.0),
        atMeters(0.0, width),
        atMeters(height, width),
        atMeters(height, 0.0),
    };
}

double localDistance(const SurveyGridCoordinate &first,
                     const SurveyGridCoordinate &second)
{
    const double latitude = (first.latitude + second.latitude) * 0.5
        / kRadiansToDegrees;
    const double north = (second.latitude - first.latitude)
        / kRadiansToDegrees * kEarthRadiusMeters;
    const double east = (second.longitude - first.longitude)
        / kRadiansToDegrees * kEarthRadiusMeters * std::cos(latitude);
    return std::hypot(north, east);
}

} // namespace

Q_DECLARE_METATYPE(SurveyGridOptions)
Q_DECLARE_METATYPE(QVector<SurveyGridCoordinate>)

class SurveyGridGeneratorTest final : public QObject
{
    Q_OBJECT

private slots:
    void createsMissionPlannerTaggedLawnmowerPath();
    void angleAndCrossGridAreDeterministic();
    void supportsMissionPlannerNegativeOvershoot();
    void appliesMissionPlannerDirectionalLeadins();
    void keepsConcavePolygonSegments();
    void handlesAntimeridianPolygon();
    void rejectsInvalidInputs_data();
    void rejectsInvalidInputs();
};

void SurveyGridGeneratorTest::createsMissionPlannerTaggedLawnmowerPath()
{
    SurveyGridOptions options;
    options.distanceMeters = 25.0;
    options.spacingMeters = 30.0;
    options.angleDegrees = 0.0;
    options.overshoot1Meters = 10.0;
    options.overshoot2Meters = 20.0;
    options.startPosition = SurveyGridOptions::StartPosition::BottomLeft;

    const SurveyGridResult result = SurveyGridGenerator::CreateGrid(
        rectangle(), options);

    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.transects.size(), 4);
    QCOMPARE(result.path.size(), 28);
    for (int index = 0; index < result.transects.size(); ++index) {
        const SurveyGridTransect &transect = result.transects.at(index);
        QCOMPARE(transect.passIndex, 0);
        QCOMPARE(transect.points.size(), 7);
        QCOMPARE(SurveyGridGenerator::PointTag(transect.points.at(0).type),
                 QStringLiteral("S"));
        QCOMPARE(SurveyGridGenerator::PointTag(transect.points.at(1).type),
                 QStringLiteral("SM"));
        QCOMPARE(SurveyGridGenerator::PointTag(transect.points.at(2).type),
                 QStringLiteral("M"));
        QCOMPARE(SurveyGridGenerator::PointTag(transect.points.at(5).type),
                 QStringLiteral("ME"));
        QCOMPARE(SurveyGridGenerator::PointTag(transect.points.at(6).type),
                 QStringLiteral("E"));
        const double expectedOvershoot = (index % 2) == 0 ? 10.0 : 20.0;
        const double actualOvershoot = localDistance(
            transect.points.at(5).coordinate,
            transect.points.at(6).coordinate);
        QVERIFY(std::abs(actualOvershoot - expectedOvershoot) < 0.02);
        for (const SurveyGridPoint &point : transect.points) {
            QCOMPARE(point.coordinate.altitude, options.altitudeMeters);
        }
    }

    const SurveyGridCoordinate firstStart =
        result.transects.first().points.at(1).coordinate;
    const SurveyGridCoordinate secondStart =
        result.transects.at(1).points.at(1).coordinate;
    QVERIFY(firstStart.latitude <
            result.transects.first().points.at(5).coordinate.latitude);
    QVERIFY(secondStart.latitude >
            result.transects.at(1).points.at(5).coordinate.latitude);
}

void SurveyGridGeneratorTest::angleAndCrossGridAreDeterministic()
{
    SurveyGridOptions options;
    options.distanceMeters = 25.0;
    options.spacingMeters = 0.0;
    options.angleDegrees = 90.0;
    options.crossGrid = true;
    options.startPosition = SurveyGridOptions::StartPosition::TopRight;

    const SurveyGridResult first = SurveyGridGenerator::CreateGrid(
        rectangle(), options);
    const SurveyGridResult second = SurveyGridGenerator::CreateGrid(
        rectangle(), options);

    QVERIFY2(first.success, qPrintable(first.error));
    QVERIFY2(second.success, qPrintable(second.error));
    QCOMPARE(first.transects.size(), 8);
    QCOMPARE(second.path.size(), first.path.size());
    for (int index = 0; index < first.path.size(); ++index) {
        QCOMPARE(static_cast<int>(second.path.at(index).type),
                 static_cast<int>(first.path.at(index).type));
        QCOMPARE(second.path.at(index).coordinate.latitude,
                 first.path.at(index).coordinate.latitude);
        QCOMPARE(second.path.at(index).coordinate.longitude,
                 first.path.at(index).coordinate.longitude);
    }
    for (int index = 0; index < first.transects.size(); ++index) {
        QCOMPARE(first.transects.at(index).passIndex, index < 4 ? 0 : 1);
    }

    const SurveyGridTransect &eastWest = first.transects.first();
    const double northSouthChange = std::abs(
        eastWest.points.at(1).coordinate.latitude
        - eastWest.points.at(2).coordinate.latitude);
    const double eastWestChange = std::abs(
        eastWest.points.at(1).coordinate.longitude
        - eastWest.points.at(2).coordinate.longitude);
    QVERIFY(eastWestChange > northSouthChange * 1000.0);
}

void SurveyGridGeneratorTest::supportsMissionPlannerNegativeOvershoot()
{
    SurveyGridOptions options;
    options.distanceMeters = 200.0;
    options.spacingMeters = 0.0;
    options.overshoot1Meters = -10.0;
    options.startPosition = SurveyGridOptions::StartPosition::BottomLeft;

    const SurveyGridResult result = SurveyGridGenerator::CreateGrid(
        rectangle(), options);

    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.transects.size(), 1);
    const SurveyGridTransect &transect = result.transects.first();
    const SurveyGridCoordinate &surveyStart = transect.points.at(1).coordinate;
    const SurveyGridCoordinate &surveyEnd = transect.points.at(2).coordinate;
    const SurveyGridCoordinate &stripEnd = transect.points.at(3).coordinate;
    QVERIFY(localDistance(surveyEnd, stripEnd) < 0.001);
    QVERIFY(std::abs(localDistance(surveyStart, surveyEnd) - 90.0) < 0.02);
}

void SurveyGridGeneratorTest::appliesMissionPlannerDirectionalLeadins()
{
    SurveyGridOptions options;
    options.distanceMeters = 25.0;
    options.spacingMeters = 0.0;
    options.leadin1Meters = 10.0;
    options.leadin2Meters = 20.0;
    options.startPosition = SurveyGridOptions::StartPosition::BottomLeft;

    const SurveyGridResult result = SurveyGridGenerator::CreateGrid(
        rectangle(), options);

    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.transects.size(), 4);
    for (int index = 0; index < result.transects.size(); ++index) {
        const SurveyGridTransect &transect = result.transects.at(index);
        const double expectedLeadin = (index % 2) == 0 ? 10.0 : 20.0;
        const double actualLeadin = localDistance(
            transect.points.at(0).coordinate,
            transect.points.at(1).coordinate);
        QVERIFY(std::abs(actualLeadin - expectedLeadin) < 0.02);
    }

    options.distanceMeters = 200.0;
    options.leadin1Meters = -10.0;
    options.leadin2Meters = 0.0;
    const SurveyGridResult negative = SurveyGridGenerator::CreateGrid(
        rectangle(), options);
    QVERIFY2(negative.success, qPrintable(negative.error));
    QCOMPARE(negative.transects.size(), 1);
    const SurveyGridTransect &transect = negative.transects.first();
    QVERIFY(localDistance(transect.points.at(0).coordinate,
                          transect.points.at(1).coordinate) < 0.001);
    QVERIFY(std::abs(localDistance(transect.points.at(1).coordinate,
                                   transect.points.at(2).coordinate)
                     - 90.0) < 0.02);
}

void SurveyGridGeneratorTest::keepsConcavePolygonSegments()
{
    const QVector<SurveyGridCoordinate> polygon{
        atMeters(0.0, 0.0),
        atMeters(0.0, 100.0),
        atMeters(100.0, 100.0),
        atMeters(100.0, 70.0),
        atMeters(30.0, 70.0),
        atMeters(30.0, 30.0),
        atMeters(100.0, 30.0),
        atMeters(100.0, 0.0),
    };
    SurveyGridOptions options;
    options.distanceMeters = 20.0;
    options.spacingMeters = 0.0;
    options.angleDegrees = 90.0;
    options.startPosition = SurveyGridOptions::StartPosition::BottomLeft;

    const SurveyGridResult result = SurveyGridGenerator::CreateGrid(
        polygon, options);

    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(result.transects.size() > 5);
    int repeatedLaneCount = 0;
    for (int index = 1; index < result.transects.size(); ++index) {
        if (result.transects.at(index - 1).laneIndex
            == result.transects.at(index).laneIndex) {
            ++repeatedLaneCount;
        }
    }
    QVERIFY(repeatedLaneCount > 0);
}

void SurveyGridGeneratorTest::handlesAntimeridianPolygon()
{
    const QVector<SurveyGridCoordinate> polygon{
        {10.0000, 179.9995, 0.0},
        {10.0000, -179.9995, 0.0},
        {10.0010, -179.9995, 0.0},
        {10.0010, 179.9995, 0.0},
    };
    SurveyGridOptions options;
    options.distanceMeters = 30.0;
    options.spacingMeters = 0.0;
    options.startPosition = SurveyGridOptions::StartPosition::BottomLeft;

    const SurveyGridResult result = SurveyGridGenerator::CreateGrid(
        polygon, options);

    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(!result.path.isEmpty());
    for (const SurveyGridPoint &point : result.path) {
        QVERIFY(point.coordinate.longitude >= -180.0);
        QVERIFY(point.coordinate.longitude <= 180.0);
    }
}

void SurveyGridGeneratorTest::rejectsInvalidInputs_data()
{
    QTest::addColumn<QVector<SurveyGridCoordinate>>("polygon");
    QTest::addColumn<SurveyGridOptions>("options");
    QTest::addColumn<QString>("expectedError");

    SurveyGridOptions options;
    QTest::newRow("too-few-vertices")
        << QVector<SurveyGridCoordinate>{atMeters(0, 0), atMeters(0, 10)}
        << options << QStringLiteral("at least three");

    QVector<SurveyGridCoordinate> nonFinite = rectangle();
    nonFinite[1].latitude = std::numeric_limits<double>::quiet_NaN();
    QTest::newRow("non-finite-coordinate")
        << nonFinite << options << QStringLiteral("not finite");

    QVector<SurveyGridCoordinate> invalidLatitude = rectangle();
    invalidLatitude[1].latitude = 91.0;
    QTest::newRow("latitude-range")
        << invalidLatitude << options << QStringLiteral("latitude outside");

    SurveyGridOptions zeroDistance = options;
    zeroDistance.distanceMeters = 0.0;
    QTest::newRow("zero-distance")
        << rectangle() << zeroDistance << QStringLiteral("greater than zero");

    SurveyGridOptions negativeSpacing = options;
    negativeSpacing.spacingMeters = -1.0;
    QTest::newRow("negative-spacing")
        << rectangle() << negativeSpacing << QStringLiteral("zero or greater");

    SurveyGridOptions nonFiniteOvershoot = options;
    nonFiniteOvershoot.overshoot1Meters =
        std::numeric_limits<double>::infinity();
    QTest::newRow("non-finite-overshoot")
        << rectangle() << nonFiniteOvershoot << QStringLiteral("finite");

    SurveyGridOptions nonFiniteLeadin = options;
    nonFiniteLeadin.leadin2Meters =
        std::numeric_limits<double>::quiet_NaN();
    QTest::newRow("non-finite-leadin")
        << rectangle() << nonFiniteLeadin << QStringLiteral("lead-in");

    const QVector<SurveyGridCoordinate> bowTie{
        atMeters(0, 0), atMeters(100, 100),
        atMeters(0, 100), atMeters(100, 0),
    };
    QTest::newRow("self-intersection")
        << bowTie << options << QStringLiteral("self-intersecting");

    const QVector<SurveyGridCoordinate> collinear{
        atMeters(0, 0), atMeters(10, 10), atMeters(20, 20),
    };
    QTest::newRow("degenerate")
        << collinear << options << QStringLiteral("degenerate");
}

void SurveyGridGeneratorTest::rejectsInvalidInputs()
{
    QFETCH(QVector<SurveyGridCoordinate>, polygon);
    QFETCH(SurveyGridOptions, options);
    QFETCH(QString, expectedError);

    const SurveyGridResult result = SurveyGridGenerator::CreateGrid(
        polygon, options);

    QVERIFY(!result.success);
    QVERIFY2(result.error.contains(expectedError, Qt::CaseInsensitive),
             qPrintable(result.error));
    QVERIFY(result.path.isEmpty());
    QVERIFY(result.transects.isEmpty());
}

QTEST_APPLESS_MAIN(SurveyGridGeneratorTest)
#include "test_surveygridgenerator.moc"
