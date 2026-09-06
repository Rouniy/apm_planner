#include "ui/Loghandling/DataFlashGpxExporter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtEndian>
#include <QtTest/QtTest>

#include <cmath>
#include <cstring>

namespace {

constexpr quint8 GpsId = 150;
constexpr quint8 Gps2Id = 151;
constexpr quint8 FmtuId = 152;
constexpr quint8 GpsLength = 30;

QByteArray fixedField(const QByteArray &text, int size)
{
    QByteArray field(size, '\0');
    field.replace(0, qMin(size, text.size()), text.left(size));
    return field;
}

template<typename Integer>
void appendLittle(QByteArray *bytes, Integer value)
{
    uchar encoded[sizeof(Integer)]{};
    qToLittleEndian<Integer>(value, encoded);
    bytes->append(reinterpret_cast<const char *>(encoded), sizeof(encoded));
}

void appendFloat(QByteArray *bytes, float value)
{
    quint32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendLittle(bytes, bits);
}

void appendFmt(QByteArray *bytes, quint8 id, quint8 length,
               const QByteArray &name, const QByteArray &format,
               const QByteArray &columns)
{
    bytes->append(char(0xa3));
    bytes->append(char(0x95));
    bytes->append(char(0x80));
    bytes->append(char(id));
    bytes->append(char(length));
    bytes->append(fixedField(name, 4));
    bytes->append(fixedField(format, 16));
    bytes->append(fixedField(columns, 64));
}

void appendFmtHeader(QByteArray *bytes)
{
    appendFmt(bytes, 128, 89, "FMT", "BBnNZ",
              "Type,Length,Name,Format,Columns");
}

void appendGpsDefinition(QByteArray *bytes, quint8 type,
                         const QByteArray &name)
{
    appendFmt(bytes, type, GpsLength, name, "QBIHLLf",
              "TimeUS,Status,GMS,GWk,Lat,Lng,Alt");
}

void appendFmtuDefinition(QByteArray *bytes)
{
    appendFmt(bytes, FmtuId, 44, "FMTU", "QBNN",
              "TimeUS,FmtType,UnitIds,MultIds");
}

void appendFmtu(QByteArray *bytes, quint8 type)
{
    bytes->append(char(0xa3));
    bytes->append(char(0x95));
    bytes->append(char(FmtuId));
    appendLittle(bytes, quint64(0));
    bytes->append(char(type));
    bytes->append(fixedField("#------", 16));
    bytes->append(fixedField("-------", 16));
}

void appendGps(QByteArray *bytes, quint8 type, quint64 timeUs,
               quint8 status, quint32 gms, quint16 week,
               qint32 latitudeE7, qint32 longitudeE7, float altitude)
{
    bytes->append(char(0xa3));
    bytes->append(char(0x95));
    bytes->append(char(type));
    appendLittle(bytes, timeUs);
    bytes->append(char(status));
    appendLittle(bytes, gms);
    appendLittle(bytes, week);
    appendLittle(bytes, latitudeE7);
    appendLittle(bytes, longitudeE7);
    appendFloat(bytes, altitude);
}

QByteArray binaryTrack()
{
    QByteArray bytes;
    appendFmtHeader(&bytes);
    appendGpsDefinition(&bytes, Gps2Id, "GPS2");
    appendGpsDefinition(&bytes, GpsId, "GPS");
    appendFmtuDefinition(&bytes);
    // MP10 scans FMTU instance types in insertion order, so GPS2 deliberately
    // establishes the one shared clock before ReadTrack enumerates GPS only.
    appendFmtu(&bytes, Gps2Id);
    appendFmtu(&bytes, GpsId);
    appendGps(&bytes, Gps2Id, 2000000, 3, 200000, 2300,
              470000000, 80000000, 1.0f);
    appendGps(&bytes, GpsId, 3000000, 2, 300000, 2300,
              473977400, 85455900, 100.0f); // no fix
    appendGps(&bytes, GpsId, 5000500, 3, 500000, 2300,
              473977419, 85455938, 273.39062f);
    appendGps(&bytes, GpsId, 6500500, 4, 999000, 2300,
              473977429, 85455948, -0.020507812f);
    appendGps(&bytes, GpsId, 7000000, 3, 700000, 2300,
              0, 0, 5.0f);
    appendGps(&bytes, GpsId, 8000000, 3, 800000, 2300,
              950000000, 85455948, 5.0f);
    return bytes;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size() && file.flush();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QStringList stagedFiles(const QString &directory)
{
    return QDir(directory).entryList(
        QStringList() << QStringLiteral(".apm-dataflash-gpx-*.partial"),
        QDir::Files | QDir::Hidden);
}

struct Point
{
    QString latitude;
    QString longitude;
    QString altitude;
    QString time;
};

struct Document
{
    bool valid = false;
    QString version;
    QString creator;
    QString rootNamespace;
    QStringList elements;
    QVector<Point> points;
};

Document parseGpx(const QByteArray &bytes)
{
    Document document;
    QXmlStreamReader reader(bytes);
    Point point;
    bool inPoint = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            document.elements.append(reader.name().toString());
            if (reader.name() == QLatin1String("gpx")) {
                document.version = reader.attributes().value(
                    QLatin1String("version")).toString();
                document.creator = reader.attributes().value(
                    QLatin1String("creator")).toString();
                document.rootNamespace = reader.namespaceUri().toString();
            } else if (reader.name() == QLatin1String("trkpt")) {
                inPoint = true;
                point = {};
                point.latitude = reader.attributes().value(
                    QLatin1String("lat")).toString();
                point.longitude = reader.attributes().value(
                    QLatin1String("lon")).toString();
            } else if (inPoint && reader.name() == QLatin1String("ele")) {
                point.altitude = reader.readElementText();
            } else if (inPoint && reader.name() == QLatin1String("time")) {
                point.time = reader.readElementText();
            }
        } else if (reader.isEndElement()
                   && reader.name() == QLatin1String("trkpt")) {
            document.points.append(point);
            inPoint = false;
        }
    }
    document.valid = !reader.hasError();
    return document;
}

} // namespace

class DataFlashGpxExporterTest final : public QObject
{
    Q_OBJECT

private slots:
    void binaryTrackUsesGpsOnlyAndOneAnchoredClock();
    void asciiTimeMsPrecedesTimeUsLikeDfItem();
    void noGpsEpochKeepsReferenceYearOneFallback();
    void emptyTrackIsAValidDocument();
    void malformedInputPublishesNothing();
    void cancellationAndSourceMutationRemovePrivateStage();
    void existingAndRacingDestinationsAreNeverOverwritten();
    void inputOutputAndSymlinkPreflightIsFailClosed();
};

void DataFlashGpxExporterTest::binaryTrackUsesGpsOnlyAndOneAnchoredClock()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("ordered.bin"));
    const QString output = directory.filePath(QStringLiteral("ordered.gpx"));
    const QByteArray source = binaryTrack();
    QVERIFY(writeFile(input, source));

    QVector<QPair<qint64, qint64>> progress;
    const auto result = DataFlashGpxExporter::Export(
        input, output, {}, [&progress](qint64 done, qint64 total) {
            progress.append(qMakePair(done, total));
        });
    QVERIFY2(result.success, qPrintable(result.error));
    QVERIFY(!result.cancelled);
    QCOMPARE(result.outputPath, QFileInfo(output).absoluteFilePath());
    QCOMPARE(result.pointCount, qint64(2));
    QCOMPARE(readFile(input), source);
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.first().first, qint64(0));
    QCOMPARE(progress.last().first, progress.last().second);
    for (int index = 1; index < progress.size(); ++index) {
        QVERIFY(progress.at(index).first >= progress.at(index - 1).first);
        QCOMPARE(progress.at(index).second, progress.first().second);
    }

    const Document document = parseGpx(readFile(output));
    QVERIFY(document.valid);
    QCOMPARE(document.version, QStringLiteral("1.1"));
    QCOMPARE(document.creator, QStringLiteral("Mission Planner"));
    QCOMPARE(document.rootNamespace,
             QStringLiteral("http://www.topografix.com/GPX/1/1"));
    QCOMPARE(document.elements.count(QStringLiteral("gpx")), 1);
    QCOMPARE(document.elements.count(QStringLiteral("trk")), 1);
    QCOMPARE(document.elements.count(QStringLiteral("trkseg")), 1);
    QVERIFY(!document.elements.contains(QStringLiteral("name")));
    QCOMPARE(document.points.size(), 2);
    QCOMPARE(document.points.at(0).latitude, QStringLiteral("47.3977419"));
    QCOMPARE(document.points.at(0).longitude, QStringLiteral("8.5455938"));
    QCOMPARE(document.points.at(0).altitude, QStringLiteral("273.39062"));
    QCOMPARE(document.points.at(1).latitude, QStringLiteral("47.3977429"));
    QCOMPARE(document.points.at(1).longitude, QStringLiteral("8.5455948"));
    QCOMPARE(document.points.at(1).altitude, QStringLiteral("-0.020507812"));

    const QDateTime anchor = QDateTime(
        QDate(1980, 1, 6), QTime(0, 0), Qt::UTC)
        .addDays(2300LL * 7).addMSecs(200000).addSecs(-18);
    QCOMPARE(document.points.at(0).time,
             anchor.addMSecs(3000).toString(
                 QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")));
    QCOMPARE(document.points.at(1).time,
             anchor.addMSecs(4500).toString(
                 QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")));
    QVERIFY(result.warnings.join(QLatin1Char(' ')).contains(
        QStringLiteral("without a valid 3D fix")));
}

void DataFlashGpxExporterTest::asciiTimeMsPrecedesTimeUsLikeDfItem()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("clock.log"));
    const QString output = directory.filePath(QStringLiteral("clock.gpx"));
    const QByteArray source =
        "FMT,128,89,FMT,BBnNZ,Type,Length,Name,Format,Columns\n"
        "FMT,150,34,GPS,QIBIHLLf,TimeUS,TimeMS,Status,GMS,GWk,Lat,Lng,Alt\n"
        "GPS,10000000,1000,3,300000,2300,47.5,8.5,12.25\n"
        "GPS,12000000,2500,3,999000,2300,47.6,8.6,13.5\n";
    QVERIFY(writeFile(input, source));

    const auto result = DataFlashGpxExporter::Export(input, output);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.pointCount, qint64(2));
    const Document document = parseGpx(readFile(output));
    QVERIFY(document.valid);
    QCOMPARE(document.points.size(), 2);
    const QDateTime anchor = QDateTime(
        QDate(1980, 1, 6), QTime(0, 0), Qt::UTC)
        .addDays(2300LL * 7).addMSecs(1000).addSecs(-18);
    // Actual MP10 oracle: TimeMS precedes GMS for the GPS epoch too.
    // TimeMS is the DFItem clock, while the initial msoffset is still the
    // integer TimeUS/1000 value from the anchor record.
    QCOMPARE(document.points.at(0).time,
             anchor.addSecs(-9).toString(
                 QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")));
    QCOMPARE(document.points.at(1).time,
             anchor.addMSecs(-7500).toString(
                 QStringLiteral("yyyy-MM-dd'T'HH:mm:ss'Z'")));
}

void DataFlashGpxExporterTest::noGpsEpochKeepsReferenceYearOneFallback()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("relative.log"));
    const QString output = directory.filePath(QStringLiteral("relative.gpx"));
    const QByteArray source =
        "FMT,128,89,FMT,BBnNZ,Type,Length,Name,Format,Columns\n"
        "FMT,150,24,GPS,QBLLf,TimeUS,Status,Lat,Lng,Alt\n"
        "GPS,0,3,47.5,8.5,12.25\n";
    QVERIFY(writeFile(input, source));

    const auto result = DataFlashGpxExporter::Export(input, output);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.pointCount, qint64(1));
    QVERIFY(result.warnings.join(QLatin1Char(' ')).contains(
        QStringLiteral("year-0001")));
    const Document document = parseGpx(readFile(output));
    QVERIFY(document.valid);
    QVERIFY(document.points.at(0).time.startsWith(QStringLiteral("0001-01-01T")));
    QVERIFY(document.points.at(0).time.endsWith(QLatin1Char('Z')));

    // Out-of-range DateTime values must fail without publishing malformed
    // year-10000+ XML or converting a rounded Double(2^63) to signed Int64.
    for (const QByteArray &clock : {QByteArray("922337203685478"),
                                  QByteArray("320000000000000")}) {
        const QString badInput = directory.filePath(QString::fromLatin1(clock) + ".log");
        const QString badOutput = directory.filePath(QString::fromLatin1(clock) + ".gpx");
        QVERIFY(writeFile(badInput,
            "FMT,128,89,FMT,BBnNZ,Type,Length,Name,Format,Columns\n"
            "FMT,150,24,GPS,QBLLf,TimeMS,Status,Lat,Lng,Alt\n"
            "GPS," + clock + ",3,47.5,8.5,12.25\n"));
        const auto invalid = DataFlashGpxExporter::Export(badInput, badOutput);
        QVERIFY(!invalid.success);
        QVERIFY(invalid.error.contains(QStringLiteral("timestamp")));
        QVERIFY(!QFile::exists(badOutput));
        QVERIFY(stagedFiles(directory.path()).isEmpty());
    }
}

void DataFlashGpxExporterTest::emptyTrackIsAValidDocument()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("empty.bin"));
    const QString output = directory.filePath(QStringLiteral("empty.gpx"));
    QByteArray source;
    appendFmtHeader(&source);
    appendGpsDefinition(&source, GpsId, "GPS");
    appendGps(&source, GpsId, 1000, 2, 1000, 2300,
              473977419, 85455938, 10.0f);
    QVERIFY(writeFile(input, source));

    const auto result = DataFlashGpxExporter::Export(input, output);
    QVERIFY2(result.success, qPrintable(result.error));
    QCOMPARE(result.pointCount, qint64(0));
    const Document document = parseGpx(readFile(output));
    QVERIFY(document.valid);
    QVERIFY(document.points.isEmpty());
    QCOMPARE(document.elements.count(QStringLiteral("trkseg")), 1);
}

void DataFlashGpxExporterTest::malformedInputPublishesNothing()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("bad.bin"));
    const QString output = directory.filePath(QStringLiteral("bad.gpx"));
    QVERIFY(writeFile(input, QByteArray::fromHex("a39501")));

    const auto result = DataFlashGpxExporter::Export(input, output);
    QVERIFY(!result.success);
    QVERIFY(!result.cancelled);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(!QFileInfo::exists(output));
    QVERIFY(stagedFiles(directory.path()).isEmpty());
}

void DataFlashGpxExporterTest::cancellationAndSourceMutationRemovePrivateStage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("cancel.bin"));
    const QString output = directory.filePath(QStringLiteral("cancel.gpx"));
    const QByteArray source = binaryTrack();
    QVERIFY(writeFile(input, source));

    int polls = 0;
    const auto cancelled = DataFlashGpxExporter::Export(
        input, output, [&polls] { return ++polls > 5; });
    QVERIFY(!cancelled.success);
    QVERIFY(cancelled.cancelled);
    QVERIFY(!QFileInfo::exists(output));
    QVERIFY(stagedFiles(directory.path()).isEmpty());

    bool changed = false;
    QByteArray replacement = source;
    replacement[replacement.size() - 1] = char(
        replacement.at(replacement.size() - 1) ^ char(1));
    const auto mutated = DataFlashGpxExporter::Export(
        input, output, {}, [&](qint64 done, qint64 total) {
            if (!changed && total > 0 && done >= total / 4) {
                changed = true;
                QVERIFY(writeFile(input, replacement));
            }
        });
    QVERIFY(changed);
    QVERIFY(!mutated.success);
    QVERIFY(mutated.error.contains(QStringLiteral("changed")));
    QVERIFY(!QFileInfo::exists(output));
    QVERIFY(stagedFiles(directory.path()).isEmpty());
}

void DataFlashGpxExporterTest::existingAndRacingDestinationsAreNeverOverwritten()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("source.bin"));
    const QString output = directory.filePath(QStringLiteral("source.gpx"));
    const QByteArray source = binaryTrack();
    const QByteArray sentinel("foreign destination");
    QVERIFY(writeFile(input, source));
    QVERIFY(writeFile(output, sentinel));

    auto result = DataFlashGpxExporter::Export(input, output);
    QVERIFY(!result.success);
    QCOMPARE(readFile(output), sentinel);
    QVERIFY(QFile::remove(output));

    bool raced = false;
    result = DataFlashGpxExporter::Export(
        input, output, {}, [&](qint64 done, qint64 total) {
            if (!raced && done == total) {
                raced = true;
                QVERIFY(writeFile(output, sentinel));
            }
        });
    QVERIFY(raced);
    QVERIFY(!result.success);
    QCOMPARE(readFile(output), sentinel);
    QVERIFY(stagedFiles(directory.path()).isEmpty());
}

void DataFlashGpxExporterTest::inputOutputAndSymlinkPreflightIsFailClosed()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("source.bin"));
    QVERIFY(writeFile(input, binaryTrack()));

    auto result = DataFlashGpxExporter::Export(input, input);
    QVERIFY(!result.success);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(readFile(input), binaryTrack());
    QVERIFY(!DataFlashGpxExporter::Export(
        directory.filePath(QStringLiteral("wrong.txt")),
        directory.filePath(QStringLiteral("wrong.gpx"))).success);
    QVERIFY(!DataFlashGpxExporter::Export(
        input, directory.filePath(QStringLiteral("wrong.xml"))).success);

#ifdef Q_OS_UNIX
    const QString inputLink = directory.filePath(QStringLiteral("linked.bin"));
    QVERIFY(QFile::link(input, inputLink));
    result = DataFlashGpxExporter::Export(
        inputLink, directory.filePath(QStringLiteral("linked.gpx")));
    QVERIFY(!result.success);

    const QString outputTarget = directory.filePath(QStringLiteral("target"));
    const QString outputLink = directory.filePath(QStringLiteral("output.gpx"));
    QVERIFY(writeFile(outputTarget, QByteArrayLiteral("target")));
    QVERIFY(QFile::link(outputTarget, outputLink));
    result = DataFlashGpxExporter::Export(input, outputLink);
    QVERIFY(!result.success);
    QCOMPARE(readFile(outputTarget), QByteArrayLiteral("target"));
#endif
}

QTEST_GUILESS_MAIN(DataFlashGpxExporterTest)

#include "test_dataflashgpxexporter.moc"
