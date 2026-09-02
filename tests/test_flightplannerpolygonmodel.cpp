#include "ui/flightplanner/FlightPlannerPolygonModel.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

namespace
{
constexpr double kEarthRadiusMeters = 6378137.0;
constexpr double kRadiansToDegrees = 57.295779513082320877;

SurveyGridCoordinate atMeters(double north, double east,
                              double altitude = 0.0,
                              double latitude = 47.0,
                              double longitude = 8.0)
{
    SurveyGridCoordinate coordinate;
    coordinate.latitude = latitude
            + north * kRadiansToDegrees / kEarthRadiusMeters;
    coordinate.longitude = longitude
            + east * kRadiansToDegrees
                    / (kEarthRadiusMeters
                       * std::cos(latitude / kRadiansToDegrees));
    coordinate.altitude = altitude;
    return coordinate;
}

QVector<SurveyGridCoordinate> rectangle(double width = 100.0,
                                        double height = 50.0)
{
    return {
        atMeters(0.0, 0.0, 10.0),
        atMeters(0.0, width, 20.0),
        atMeters(height, width, 30.0),
        atMeters(height, 0.0, 40.0),
    };
}

WpRowData waypoint(double north, double east, quint16 command = 16,
                   quint8 frame = 3)
{
    const SurveyGridCoordinate coordinate = atMeters(north, east, 55.0);
    WpRowData row;
    row.Command = command;
    row.Frame = frame;
    row.Lat = coordinate.latitude;
    row.Lng = coordinate.longitude;
    row.Alt = coordinate.altitude;
    return row;
}
}

class FlightPlannerPolygonModelTest final : public QObject
{
    Q_OBJECT

private slots:
    void roundTripsLegacyPolyAndBuildsFromFlightPath();
    void reportsAreaAndOffsetsInMeters();
    void allowsCollinearThirdPointWhileDrawing();
    void invalidInputDoesNotMutateStateOrTargetFile();
};

void FlightPlannerPolygonModelTest::roundTripsLegacyPolyAndBuildsFromFlightPath()
{
    FlightPlannerPolygonModel source;
    QVERIFY(source.ReplaceDrawnPolygon(rectangle()));
    QCOMPARE(source.Count(), 4);

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString path = temporaryDirectory.filePath(QStringLiteral("field.poly"));
    QVERIFY(source.SavePolygon(path));

    QFile saved(path);
    QVERIFY(saved.open(QIODevice::ReadOnly));
    const QList<QByteArray> savedLines = saved.readAll().trimmed().split('\n');
    QCOMPARE(savedLines.size(), 6); // header, four vertices, closing vertex
    QCOMPARE(savedLines.at(1), savedLines.at(5));

    FlightPlannerPolygonModel loaded;
    QSignalSpy changedSpy(&loaded,
                          &FlightPlannerPolygonModel::DrawnPolygonChanged);
    QVERIFY(loaded.LoadPolygon(path));
    QCOMPARE(changedSpy.count(), 1);
    QCOMPARE(loaded.Count(), 4);
    for (int index = 0; index < loaded.Count(); ++index) {
        QCOMPARE(loaded.DrawnPolygon().at(index).latitude,
                 source.DrawnPolygon().at(index).latitude);
        QCOMPARE(loaded.DrawnPolygon().at(index).longitude,
                 source.DrawnPolygon().at(index).longitude);
        // Mission Planner's legacy .poly format stores latitude/longitude
        // only; a caller supplies its current default altitude after import.
        QCOMPARE(loaded.DrawnPolygon().at(index).altitude, 0.0);
    }

    const QVector<WpRowData> rows{
        waypoint(0.0, 0.0),
        waypoint(0.0, 100.0),
        waypoint(20.0, 20.0, 178, 3), // global, but not a flight path
        waypoint(50.0, 100.0, 16, 1), // flight path, but local frame
        waypoint(50.0, 100.0, 82, 3),
        waypoint(50.0, 0.0),
    };
    QVERIFY(loaded.BuildPolygonFromWaypoints(rows));
    QCOMPARE(loaded.Count(), 4);
    QCOMPARE(loaded.DrawnPolygon().at(2).altitude, 55.0);
}

void FlightPlannerPolygonModelTest::reportsAreaAndOffsetsInMeters()
{
    FlightPlannerPolygonModel model;
    QVERIFY(model.ReplaceDrawnPolygon(rectangle(100.0, 100.0)));
    QVERIFY(std::abs(model.PolygonArea() - 10000.0) < 0.1);

    QVERIFY(model.OffsetDrawnPolygon(10.0));
    QVERIFY2(std::abs(model.PolygonArea() - 14400.0) < 0.5,
             qPrintable(QString::number(model.PolygonArea(), 'f', 6)));
    QCOMPARE(model.Count(), 4);

    QVERIFY(model.ReplaceDrawnPolygon(rectangle(100.0, 100.0)));
    QVERIFY(model.OffsetDrawnPolygon(-10.0));
    QVERIFY(std::abs(model.PolygonArea() - 6400.0) < 0.5);

    const QVector<SurveyGridCoordinate> beforeCollapse = model.DrawnPolygon();
    QVERIFY(!model.OffsetDrawnPolygon(-50.0));
    QCOMPARE(model.DrawnPolygon().size(), beforeCollapse.size());
    for (int index = 0; index < beforeCollapse.size(); ++index) {
        QCOMPARE(model.DrawnPolygon().at(index).latitude,
                 beforeCollapse.at(index).latitude);
        QCOMPARE(model.DrawnPolygon().at(index).longitude,
                 beforeCollapse.at(index).longitude);
    }
}

void FlightPlannerPolygonModelTest::allowsCollinearThirdPointWhileDrawing()
{
    FlightPlannerPolygonModel model;
    QVERIFY(model.AddDrawnPolygonPoint(atMeters(0.0, 0.0)));
    QVERIFY(model.AddDrawnPolygonPoint(atMeters(20.0, 0.0)));
    QVERIFY(model.AddDrawnPolygonPoint(atMeters(40.0, 0.0)));
    QCOMPARE(model.Count(), 3);
    QVERIFY(!model.IsValid());
    QVERIFY(model.AddDrawnPolygonPoint(atMeters(40.0, 20.0)));
    QVERIFY(model.IsValid());
}

void FlightPlannerPolygonModelTest::invalidInputDoesNotMutateStateOrTargetFile()
{
    FlightPlannerPolygonModel model;
    QVERIFY(model.ReplaceDrawnPolygon(rectangle()));
    const QVector<SurveyGridCoordinate> original = model.DrawnPolygon();

    const QVector<SurveyGridCoordinate> bowTie{
        atMeters(0.0, 0.0),
        atMeters(50.0, 100.0),
        atMeters(0.0, 100.0),
        atMeters(50.0, 0.0),
    };
    QSignalSpy errorSpy(&model,
                        &FlightPlannerPolygonModel::errorOccurred);
    QVERIFY(!model.ReplaceDrawnPolygon(bowTie));
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(model.DrawnPolygon().size(), original.size());
    QCOMPARE(model.DrawnPolygon().first().latitude,
             original.first().latitude);

    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());
    const QString invalidPath = temporaryDirectory.filePath(
            QStringLiteral("invalid.poly"));
    QFile invalid(invalidPath);
    QVERIFY(invalid.open(QIODevice::WriteOnly));
    invalid.write("# a self-intersecting polygon\n"
                  "47.0 8.0\n"
                  "47.001 8.002\n"
                  "47.0 8.002\n"
                  "47.001 8.0\n");
    invalid.close();
    QVERIFY(!model.LoadPolygon(invalidPath));
    QCOMPARE(model.DrawnPolygon().size(), original.size());

    const QString targetPath = temporaryDirectory.filePath(
            QStringLiteral("existing.poly"));
    QFile target(targetPath);
    QVERIFY(target.open(QIODevice::WriteOnly));
    QCOMPARE(target.write("sentinel\n"), qint64(9));
    target.close();

    FlightPlannerPolygonModel empty;
    QVERIFY(!empty.SavePolygon(targetPath));
    QVERIFY(target.open(QIODevice::ReadOnly));
    QCOMPARE(target.readAll(), QByteArray("sentinel\n"));
}

QTEST_MAIN(FlightPlannerPolygonModelTest)
#include "test_flightplannerpolygonmodel.moc"
