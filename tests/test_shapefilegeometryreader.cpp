#include "services/ShapefileGeometryReader.h"

#include <QtTest>

#include <cstring>

namespace {
void u16le(QByteArray *bytes, quint16 value)
{
    bytes->append(char(value & 0xff));
    bytes->append(char((value >> 8) & 0xff));
}

void u32le(QByteArray *bytes, quint32 value)
{
    bytes->append(char(value & 0xff));
    bytes->append(char((value >> 8) & 0xff));
    bytes->append(char((value >> 16) & 0xff));
    bytes->append(char((value >> 24) & 0xff));
}

void u32be(QByteArray *bytes, quint32 value)
{
    bytes->append(char((value >> 24) & 0xff));
    bytes->append(char((value >> 16) & 0xff));
    bytes->append(char((value >> 8) & 0xff));
    bytes->append(char(value & 0xff));
}

void f64le(QByteArray *bytes, double value)
{
    quint64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int index = 0; index < 8; ++index)
        bytes->append(char((bits >> (8 * index)) & 0xff));
}

QByteArray pointRecord(quint32 type, double x, double y,
                       double z = 0, bool includeM = true)
{
    QByteArray record;
    u32le(&record, type);
    f64le(&record, x);
    f64le(&record, y);
    if (type == 11) f64le(&record, z);
    if (type == 21 || (type == 11 && includeM)) f64le(&record, 123.0);
    return record;
}

void appendRange(QByteArray *record)
{
    f64le(record, 0);
    f64le(record, 1);
}

QByteArray multiPointRecord(quint32 type,
                            const QVector<Shapefile::Coordinate> &points,
                            bool includeM = true)
{
    QByteArray record;
    u32le(&record, type);
    for (int index = 0; index < 4; ++index) f64le(&record, 0);
    u32le(&record, points.size());
    for (const auto &point : points) {
        f64le(&record, point.x);
        f64le(&record, point.y);
    }
    if (type == 18) {
        appendRange(&record);
        for (const auto &point : points) f64le(&record, point.z);
    }
    if (type == 28 || (type == 18 && includeM)) {
        appendRange(&record);
        for (int index = 0; index < points.size(); ++index) f64le(&record, index);
    }
    return record;
}

QByteArray multiPartRecord(quint32 type,
                           const QVector<QVector<Shapefile::Coordinate>> &parts,
                           bool includeM = true)
{
    QByteArray record;
    u32le(&record, type);
    for (int index = 0; index < 4; ++index) f64le(&record, 0);
    int pointCount = 0;
    for (const auto &part : parts) pointCount += part.size();
    u32le(&record, parts.size());
    u32le(&record, pointCount);
    int offset = 0;
    for (const auto &part : parts) {
        u32le(&record, offset);
        offset += part.size();
    }
    for (const auto &part : parts) {
        for (const auto &point : part) {
            f64le(&record, point.x);
            f64le(&record, point.y);
        }
    }
    if (type == 13 || type == 15) {
        appendRange(&record);
        for (const auto &part : parts)
            for (const auto &point : part) f64le(&record, point.z);
    }
    if (type == 23 || type == 25
        || ((type == 13 || type == 15) && includeM)) {
        appendRange(&record);
        for (int index = 0; index < pointCount; ++index) f64le(&record, index);
    }
    return record;
}

QByteArray shp(quint32 type, const QVector<QByteArray> &records)
{
    QByteArray bytes;
    u32be(&bytes, 9994);
    bytes.append(20, '\0');
    u32be(&bytes, 0);
    u32le(&bytes, 1000);
    u32le(&bytes, type);
    bytes.append(64, '\0');
    quint32 number = 1;
    for (const QByteArray &record : records) {
        u32be(&bytes, number++);
        u32be(&bytes, record.size() / 2);
        bytes.append(record);
    }
    const quint32 words = bytes.size() / 2;
    bytes[24] = char((words >> 24) & 0xff);
    bytes[25] = char((words >> 16) & 0xff);
    bytes[26] = char((words >> 8) & 0xff);
    bytes[27] = char(words & 0xff);
    return bytes;
}

QByteArray dbf(const QVector<QPair<bool, QByteArray>> &records,
               char type = 'N', int fieldLength = 4)
{
    const quint16 headerSize = 65;
    const quint16 recordSize = 1 + fieldLength;
    QByteArray bytes;
    bytes.append(char(3));
    bytes.append(char(124));
    bytes.append(char(1));
    bytes.append(char(1));
    u32le(&bytes, records.size());
    u16le(&bytes, headerSize);
    u16le(&bytes, recordSize);
    bytes.append(20, '\0');
    const QByteArray name = QByteArray("VALUE").leftJustified(10, '\0', true);
    bytes.append(name);
    bytes.append('\0');
    bytes.append(type);
    bytes.append(4, '\0');
    bytes.append(char(fieldLength));
    bytes.append('\0');
    bytes.append(14, '\0');
    bytes.append(char(0x0d));
    for (const auto &record : records) {
        bytes.append(record.first ? '*' : ' ');
        bytes.append(record.second.leftJustified(fieldLength, ' ', true));
    }
    return bytes;
}

QVector<Shapefile::Coordinate> ring(std::initializer_list<QPair<double, double>> values)
{
    QVector<Shapefile::Coordinate> result;
    for (const auto &value : values) result.append({value.first, value.second, 0});
    return result;
}
}

class ShapefileGeometryReaderTest final : public QObject
{
    Q_OBJECT
private slots:
    void readsPointNullAndIgnoresMissingShx();
    void readsEverySupportedCoordinateLayout();
    void preservesStrictPolygonPartOrder();
    void rejectsInvalidPolygonTopology();
    void appliesDbfDeletionAndCountRules();
    void rejectsMalformedDbfAndShp();
    void cancellationAndProgressAreBounded();
};

void ShapefileGeometryReaderTest::readsPointNullAndIgnoresMissingShx()
{
    QByteArray nullRecord;
    u32le(&nullRecord, 0);
    const auto result = ShapefileGeometryReader::read(
        shp(1, {pointRecord(1, 30, 40), nullRecord}), false);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.features.size(), 2);
    QCOMPARE(result.features.at(0).points.size(), 1);
    QCOMPARE(result.features.at(0).points.first().x, 30.0);
    QCOMPARE(result.features.at(0).points.first().y, 40.0);
    QVERIFY(result.features.at(1).points.isEmpty());
}

void ShapefileGeometryReaderTest::readsEverySupportedCoordinateLayout()
{
    for (const bool includeM : {false, true}) {
        const auto point = ShapefileGeometryReader::read(
            shp(11, {pointRecord(11, 1, 2, 3, includeM)}), false);
        QVERIFY2(point.success, qPrintable(point.error));
        QCOMPARE(point.features.first().points.first().z, 3.0);

        const QVector<Shapefile::Coordinate> points{{1, 2, 3}, {4, 5, 6}};
        const auto multi = ShapefileGeometryReader::read(
            shp(18, {multiPointRecord(18, points, includeM)}), false);
        QVERIFY2(multi.success, qPrintable(multi.error));
        QCOMPARE(multi.features.first().points.size(), 2);
        QCOMPARE(multi.features.first().points.at(1).z, 6.0);

        const auto line = ShapefileGeometryReader::read(shp(13, {
            multiPartRecord(13, {{points.at(0), points.at(1)}}, includeM)}), false);
        QVERIFY2(line.success, qPrintable(line.error));
        QCOMPARE(line.features.first().points.size(), 2);
    }

    const QVector<Shapefile::Coordinate> points{{7, 8, 0}, {9, 10, 0}};
    const auto measuredPoint = ShapefileGeometryReader::read(
        shp(21, {pointRecord(21, 7, 8)}), false);
    QVERIFY2(measuredPoint.success, qPrintable(measuredPoint.error));
    QCOMPARE(measuredPoint.features.first().points.size(), 1);
    for (const auto &fixture : QVector<QPair<quint32, QByteArray>>{
             {8, multiPointRecord(8, points)},
             {28, multiPointRecord(28, points)},
             {3, multiPartRecord(3, {{points.at(0), points.at(1)}})},
             {23, multiPartRecord(23, {{points.at(0), points.at(1)}})}}) {
        const auto result = ShapefileGeometryReader::read(
            shp(fixture.first, {fixture.second}), false);
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.features.first().points.size(), 2);
    }
}

void ShapefileGeometryReaderTest::preservesStrictPolygonPartOrder()
{
    // A CCW first ring is still the shell in NTS Strict mode. The following
    // CCW ring is its hole; orientation itself is not an IsValid failure.
    const auto shell = ring({{0, 0}, {4, 0}, {4, 4}, {0, 4}, {0, 0}});
    const auto hole = ring({{1, 1}, {2, 1}, {2, 2}, {1, 2}, {1, 1}});
    const auto secondShell = ring({{10, 10}, {10, 12}, {12, 12},
                                   {12, 10}, {10, 10}});
    const auto result = ShapefileGeometryReader::read(shp(5, {
        multiPartRecord(5, {shell, hole, secondShell})}), false);
    if (!result.success && result.error.contains(QStringLiteral("GDAL/GEOS"))) {
        QVERIFY(!result.error.contains(QStringLiteral("Invalid polygon")));
        QSKIP(qPrintable(result.error));
    }
    QVERIFY2(result.success, qPrintable(result.error));
    const auto points = result.features.first().points;
    QCOMPARE(points.size(), shell.size() + hole.size() + secondShell.size());
    QCOMPARE(points.at(shell.size()).x, hole.first().x);
    QCOMPARE(points.at(shell.size() + hole.size()).x, secondShell.first().x);

    for (const auto &fixture : QVector<QPair<quint32, QByteArray>>{
             {15, multiPartRecord(15, {shell}, false)},
             {15, multiPartRecord(15, {shell}, true)},
             {25, multiPartRecord(25, {shell}, true)}}) {
        const auto variant = ShapefileGeometryReader::read(
            shp(fixture.first, {fixture.second}), false);
        QVERIFY2(variant.success, qPrintable(variant.error));
        QCOMPARE(variant.features.first().points.size(), shell.size());
    }
}

void ShapefileGeometryReaderTest::rejectsInvalidPolygonTopology()
{
    const auto open = ring({{0, 0}, {0, 2}, {2, 2}, {2, 0}});
    auto result = ShapefileGeometryReader::read(
        shp(5, {multiPartRecord(5, {open})}), false);
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("closed"), Qt::CaseInsensitive));

    const auto bowTie = ring({{0, 0}, {2, 2}, {0, 2}, {2, 0}, {0, 0}});
    result = ShapefileGeometryReader::read(
        shp(5, {multiPartRecord(5, {bowTie})}), false);
    if (!result.success && result.error.contains(QStringLiteral("GDAL/GEOS")))
        QSKIP(qPrintable(result.error));
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("Invalid polygon"))
            || result.error.contains(QStringLiteral("GDAL")));
}

void ShapefileGeometryReaderTest::appliesDbfDeletionAndCountRules()
{
    const QByteArray shapes = shp(1, {
        pointRecord(1, 1, 2), pointRecord(1, 3, 4), pointRecord(1, 5, 6)});
    auto result = ShapefileGeometryReader::read(shapes, true,
        dbf({{false, "1"}, {true, "2"}, {false, "3"}}));
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.features.size(), 2);
    QCOMPARE(result.features.at(1).points.first().x, 5.0);

    // NTS caps SHP enumeration by DBF count, so extra geometries are ignored.
    result = ShapefileGeometryReader::read(shapes, true, dbf({{false, "1"}}));
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.features.size(), 1);
    QVERIFY(result.warnings.join(QLatin1Char(' ')).contains(
        QStringLiteral("remaining SHP records")));

    // Strict NTS consumes the paired DBF row while skipping a record whose
    // shape type differs from the file header.
    QByteArray mismatched;
    u32le(&mismatched, 8);
    result = ShapefileGeometryReader::read(shp(1, {
        pointRecord(1, 1, 2), mismatched, pointRecord(1, 5, 6)}), true,
        dbf({{false, "1"}, {false, "2"}, {false, "3"}}));
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.features.size(), 2);
    QVERIFY(!result.warnings.isEmpty());

    result = ShapefileGeometryReader::read(shp(1, {pointRecord(1, 1, 2)}),
        true, dbf({{false, "1"}, {false, "2"}}));
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("more records")));
}

void ShapefileGeometryReaderTest::rejectsMalformedDbfAndShp()
{
    // Deleted DBF records are omitted only after their values are parsed.
    QByteArray malformedDbf = dbf({{true, "oops"}});
    auto result = ShapefileGeometryReader::read(
        shp(1, {pointRecord(1, 1, 2)}), true, malformedDbf);
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("numeric"), Qt::CaseInsensitive));

    malformedDbf = dbf({{false, "1"}});
    malformedDbf[64] = '\0';
    result = ShapefileGeometryReader::read(
        shp(1, {pointRecord(1, 1, 2)}), true, malformedDbf);
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("terminator")));

    malformedDbf = dbf({{false, "1"}});
    malformedDbf[0] = char(0x83);
    result = ShapefileGeometryReader::read(
        shp(1, {pointRecord(1, 1, 2)}), true, malformedDbf);
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("version")));

    malformedDbf = dbf({{false, "1"}});
    malformedDbf[2] = char(13);
    result = ShapefileGeometryReader::read(
        shp(1, {pointRecord(1, 1, 2)}), true, malformedDbf);
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("date")));

    QByteArray partialM = multiPointRecord(18, {{1, 2, 3}}, false);
    partialM.append(8, '\0');
    result = ShapefileGeometryReader::read(shp(18, {partialM}), false);
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("partial")));

    QByteArray invalidVersion = shp(1, {pointRecord(1, 1, 2)});
    invalidVersion[28] = char(0);
    result = ShapefileGeometryReader::read(invalidVersion, false);
    QVERIFY(!result.success);

    result = ShapefileGeometryReader::read(
        shp(31, {QByteArray::fromHex("1f000000")}), false);
    QVERIFY(!result.success);
    QVERIFY(result.error.contains(QStringLiteral("MultiPatch")));
}

void ShapefileGeometryReaderTest::cancellationAndProgressAreBounded()
{
    int cancelCalls = 0;
    int progressCalls = 0;
    const auto result = ShapefileGeometryReader::read(shp(1, {
        pointRecord(1, 1, 2), pointRecord(1, 3, 4), pointRecord(1, 5, 6)}),
        false, {}, {}, [&] { return ++cancelCalls > 2; },
        [&](qint64 completed, qint64 total, const QString &) {
            QVERIFY(completed <= total);
            ++progressCalls;
        });
    QVERIFY(result.cancelled);
    QVERIFY(!result.success);
    QVERIFY(result.features.isEmpty());
    QCOMPARE(progressCalls, 1);

    QVector<Shapefile::Coordinate> manyPoints;
    manyPoints.resize(2049);
    cancelCalls = 0;
    const auto large = ShapefileGeometryReader::read(
        shp(8, {multiPointRecord(8, manyPoints)}), false, {}, {},
        [&] { return ++cancelCalls > 2; });
    QVERIFY(large.cancelled);
    QVERIFY(!large.success);
    QVERIFY(large.features.isEmpty());

    cancelCalls = 0;
    const auto dbfCancelled = ShapefileGeometryReader::read(
        shp(1, {pointRecord(1, 1, 2), pointRecord(1, 3, 4),
                pointRecord(1, 5, 6)}), true,
        dbf({{false, "1"}, {false, "2"}, {false, "3"}}), {},
        [&] { return ++cancelCalls > 2; });
    QVERIFY(dbfCancelled.cancelled);
    QVERIFY(!dbfCancelled.success);
    QVERIFY(dbfCancelled.features.isEmpty());
}

QTEST_GUILESS_MAIN(ShapefileGeometryReaderTest)
#include "test_shapefilegeometryreader.moc"
