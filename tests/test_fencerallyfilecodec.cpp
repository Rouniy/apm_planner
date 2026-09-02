#include "ui/flightplanner/FenceRallyFileCodec.h"

#include "QGCMAVLink.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QTest>

using namespace MissionPlanner;

class FenceRallyFileCodecTest final : public QObject
{
    Q_OBJECT

private slots:
    void legacyFenceRoundTripRemovesClosingVertex();
    void modernFenceModelRoundTripsMissionRows();
    void legacyFenceRejectsUnsupportedGeometryWithoutOverwrite();
    void legacyRallyRoundTripPreservesAllFields();
    void malformedInputReportsSourceLine();
};

void FenceRallyFileCodecTest::legacyFenceRoundTripRemovesClosingVertex()
{
    const QByteArray source = QByteArray::fromHex("efbbbf") + QByteArray(
            "# saved by Mission Planner\r\n"
            "40.0000000 28.0000000\n"
            "40.1000000 28.1000000\n"
            "40.2000000 28.2000000\n"
            "40.3000000 28.1000000\n"
            "40.1000000 28.1000000\n");

    const auto decoded = FenceRallyFileCodec::DecodeLegacyFence(source);
    QVERIFY2(decoded.ok, qPrintable(decoded.error));
    QVERIFY(decoded.fence.HasReturn);
    QCOMPARE(decoded.fence.ReturnPoint.Return.Latitude, 40.0);
    QCOMPARE(decoded.fence.ReturnPoint.Return.Longitude, 28.0);
    QCOMPARE(decoded.fence.ReturnPoint.Frame, quint8(MAV_FRAME_GLOBAL));
    QCOMPARE(decoded.fence.Polygons.size(), 1);
    QCOMPARE(decoded.fence.Polygons.first().Points.size(), 3);
    QCOMPARE(decoded.fence.Polygons.first().Mode,
             FencePolygon::PolyType::Inclusive);

    const auto encoded = FenceRallyFileCodec::EncodeLegacyFence(decoded.fence);
    QVERIFY2(encoded.ok, qPrintable(encoded.error));
    const auto roundTrip = FenceRallyFileCodec::DecodeLegacyFence(encoded.data);
    QVERIFY2(roundTrip.ok, qPrintable(roundTrip.error));
    QCOMPARE(roundTrip.fence.Polygons.first().Points.size(), 3);
    QCOMPARE(roundTrip.fence.Polygons.first().Points.at(1).Latitude, 40.2);
    QCOMPARE(roundTrip.fence.Polygons.first().Points.at(1).Longitude, 28.2);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("roundtrip.fen"));
    const auto saved = FenceRallyFileCodec::SaveLegacyFence(
            path, decoded.fence);
    QVERIFY2(saved.ok, qPrintable(saved.error));
    const auto loaded = FenceRallyFileCodec::LoadLegacyFence(path);
    QVERIFY2(loaded.ok, qPrintable(loaded.error));
    QCOMPARE(loaded.fence.Polygons.first().Points.size(), 3);
}

void FenceRallyFileCodecTest::modernFenceModelRoundTripsMissionRows()
{
    Fence fence;
    fence.HasReturn = true;
    fence.ReturnPoint.Return = {40.0, 28.0, 35.0};
    fence.ReturnPoint.Frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;

    FencePolygon inclusion;
    inclusion.Points = {
        {40.1, 28.1, 0.0},
        {40.2, 28.2, 0.0},
        {40.3, 28.1, 0.0},
    };
    fence.Polygons.append(inclusion);

    FencePolygon exclusion;
    exclusion.Mode = FencePolygon::PolyType::Exclusive;
    exclusion.Points = {
        {40.14, 28.14, 0.0},
        {40.15, 28.16, 0.0},
        {40.16, 28.14, 0.0},
    };
    fence.Polygons.append(exclusion);

    FenceCircle circle;
    circle.Mode = FenceCircle::PolyType::Exclusive;
    circle.Center = {40.25, 28.25, 0.0};
    circle.Radius = 125.5;
    fence.Circles.append(circle);

    QString error;
    const QVector<WpRowData> rows = fence.FenceToLocation(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(rows.size(), 8);
    QCOMPARE(rows.at(0).Command,
             quint16(MAV_CMD_NAV_FENCE_RETURN_POINT));
    QCOMPARE(rows.at(1).P1, 3.0);
    QCOMPARE(rows.at(4).Command,
             quint16(MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION));
    QCOMPARE(rows.last().Command,
             quint16(MAV_CMD_NAV_FENCE_CIRCLE_EXCLUSION));
    QCOMPARE(rows.last().P1, 125.5);

    const auto decoded = Fence::LocationToFence(rows);
    QVERIFY2(decoded.ok, qPrintable(decoded.error));
    QVERIFY(decoded.fence.HasReturn);
    QCOMPARE(decoded.fence.ReturnPoint.Return.Altitude, 35.0);
    QCOMPARE(decoded.fence.Polygons.size(), 2);
    QCOMPARE(decoded.fence.Polygons.at(1).Mode,
             FencePolygon::PolyType::Exclusive);
    QCOMPARE(decoded.fence.Circles.size(), 1);
    QCOMPARE(decoded.fence.Circles.first().Radius, 125.5);

    QVector<WpRowData> mapCreated = rows.mid(1, 3);
    for (WpRowData &vertex : mapCreated) {
        vertex.P1 = 0.0;
    }
    const auto inferred = Fence::LocationToFence(mapCreated);
    QVERIFY2(inferred.ok, qPrintable(inferred.error));
    QCOMPARE(inferred.fence.Polygons.size(), 1);
    QCOMPARE(inferred.fence.Polygons.first().Points.size(), 3);

    QVector<WpRowData> malformed = rows;
    malformed[2].P1 = 4;
    const auto rejected = Fence::LocationToFence(malformed);
    QVERIFY(!rejected.ok);
    QVERIFY(rejected.error.contains(QStringLiteral("P1=3")));
}

void FenceRallyFileCodecTest::legacyFenceRejectsUnsupportedGeometryWithoutOverwrite()
{
    Fence fence;
    fence.HasReturn = true;
    fence.ReturnPoint.Return = {40.0, 28.0, 0.0};
    FencePolygon polygon;
    polygon.Points = {
        {40.1, 28.1, 0.0},
        {40.2, 28.2, 0.0},
        {40.3, 28.1, 0.0},
    };
    fence.Polygons.append(polygon);
    FenceCircle circle;
    circle.Center = {40.2, 28.15, 0.0};
    circle.Radius = 50.0;
    fence.Circles.append(circle);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("fence.fen"));
    QFile existing(path);
    QVERIFY(existing.open(QIODevice::WriteOnly));
    QCOMPARE(existing.write("keep me"), qint64(7));
    existing.close();

    const auto saved = FenceRallyFileCodec::SaveLegacyFence(path, fence);
    QVERIFY(!saved.ok);
    QVERIFY(saved.error.contains(QStringLiteral("circles")));
    QVERIFY(existing.open(QIODevice::ReadOnly));
    QCOMPARE(existing.readAll(), QByteArray("keep me"));
}

void FenceRallyFileCodecTest::legacyRallyRoundTripPreservesAllFields()
{
    const QByteArray source(
            "# saved by Mission Planner\n"
            "RALLY\t40.5\t28.5\t60\t30\t27000\t3\n"
            "rally,41.5,29.5,70\n");

    const auto decoded = FenceRallyFileCodec::DecodeLegacyRally(source);
    QVERIFY2(decoded.ok, qPrintable(decoded.error));
    QCOMPARE(decoded.rally.Points.size(), 2);
    const RallyPoint &first = decoded.rally.Points.first();
    QCOMPARE(first.Position.Latitude, 40.5);
    QCOMPARE(first.Position.Longitude, 28.5);
    QCOMPARE(first.Position.Altitude, 60.0);
    QCOMPARE(first.BreakAltitude, 30.0);
    QCOMPARE(first.LandHeading, 27000.0);
    QCOMPARE(first.Flags, quint8(3));
    QCOMPARE(first.Frame, quint8(MAV_FRAME_GLOBAL_RELATIVE_ALT));
    QCOMPARE(decoded.rally.Points.at(1).BreakAltitude, 0.0);

    const auto encoded = FenceRallyFileCodec::EncodeLegacyRally(decoded.rally);
    QVERIFY2(encoded.ok, qPrintable(encoded.error));
    const auto roundTrip = FenceRallyFileCodec::DecodeLegacyRally(encoded.data);
    QVERIFY2(roundTrip.ok, qPrintable(roundTrip.error));
    QCOMPARE(roundTrip.rally.Points.size(), 2);
    QCOMPARE(roundTrip.rally.Points.first().LandHeading, 27000.0);
    QCOMPARE(roundTrip.rally.Points.first().Flags, quint8(3));

    QString error;
    const QVector<WpRowData> rows = roundTrip.rally.RallyToLocation(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(rows.first().Command, quint16(MAV_CMD_NAV_RALLY_POINT));
    QCOMPARE(rows.first().P1, 30.0);
    QCOMPARE(rows.first().P2, 27000.0);
    QCOMPARE(rows.first().P3, 3.0);
    const auto modelRoundTrip = RallyPoints::LocationToRally(rows);
    QVERIFY2(modelRoundTrip.ok, qPrintable(modelRoundTrip.error));
    QCOMPARE(modelRoundTrip.rally.Points.first().Position.Altitude, 60.0);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("roundtrip.ral"));
    const auto saved = FenceRallyFileCodec::SaveLegacyRally(
            path, decoded.rally);
    QVERIFY2(saved.ok, qPrintable(saved.error));
    const auto loaded = FenceRallyFileCodec::LoadLegacyRally(path);
    QVERIFY2(loaded.ok, qPrintable(loaded.error));
    QCOMPARE(loaded.rally.Points.first().LandHeading, 27000.0);
}

void FenceRallyFileCodecTest::malformedInputReportsSourceLine()
{
    const auto badFence = FenceRallyFileCodec::DecodeLegacyFence(
            "# comment\n40 28\n40.1 28.1\nnot-a-number 28.2\n40.3 28.1\n");
    QVERIFY(!badFence.ok);
    QVERIFY(badFence.error.contains(QStringLiteral("line 4")));

    const auto badRally = FenceRallyFileCodec::DecodeLegacyRally(
            "# comment\nRALLY 40 28 50 20 9000 2.5\n");
    QVERIFY(!badRally.ok);
    QVERIFY(badRally.error.contains(QStringLiteral("line 2")));
    QVERIFY(badRally.error.contains(QStringLiteral("flags")));
}

QTEST_APPLESS_MAIN(FenceRallyFileCodecTest)
#include "test_fencerallyfilecodec.moc"
