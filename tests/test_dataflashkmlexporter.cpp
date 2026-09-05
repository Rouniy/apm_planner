#include "ui/Loghandling/DataFlashKmlExporter.h"

#include <QByteArray>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest>

#include <cmath>
#include <limits>

namespace {

// Modern ArduCopter GPS record: TimeUS,I,Status,GMS,GWk,NSats,HDop,Lat,Lng,
// Alt,Spd,GCrs,VZ,Yaw,U with format QBBIHBcLLeffffB (48 payload bytes).
const quint8 kGpsId = 150;
const quint8 kGpsLength = 51;
const QByteArray kGpsFormat("QBBIHBcLLeffffB");
const QByteArray kGpsLabels(
    "TimeUS,I,Status,GMS,GWk,NSats,HDop,Lat,Lng,Alt,Spd,GCrs,VZ,Yaw,U");
const quint8 kFmtuId = 151;

QByteArray fixedField(const QByteArray &text, int size)
{
    QByteArray field(size, '\0');
    const QByteArray clipped = text.left(size);
    for (int index = 0; index < clipped.size(); ++index) {
        field[index] = clipped.at(index);
    }
    return field;
}

void appendFmt(QByteArray *bytes, quint8 id, quint8 length,
               const QByteArray &name, const QByteArray &format,
               const QByteArray &labels)
{
    bytes->append(char(0xA3));
    bytes->append(char(0x95));
    bytes->append(char(0x80));
    bytes->append(char(id));
    bytes->append(char(length));
    bytes->append(fixedField(name, 4));
    bytes->append(fixedField(format, 16));
    bytes->append(fixedField(labels, 64));
}

void appendGps(QByteArray *bytes, quint64 timeUs, quint8 status,
               qint32 latE7, qint32 lngE7, qint32 altCm)
{
    bytes->append(char(0xA3));
    bytes->append(char(0x95));
    bytes->append(char(kGpsId));
    QDataStream stream(bytes, QIODevice::Append);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
    stream << timeUs << quint8(0) << status << quint32(0) << quint16(0)
           << quint8(10) << qint16(120) << latE7 << lngE7 << altCm
           << 0.0F << 0.0F << 0.0F << 0.0F << quint8(1);
}

// FMTU row for the GPS type: TimeUS,FmtType,UnitIds,MultIds (QBNN). Equal
// unit/multiplier string lengths switch LogParserBase into raw-value mode.
void appendGpsFmtu(QByteArray *bytes)
{
    appendFmt(bytes, kFmtuId, 3 + 8 + 1 + 16 + 16, "FMTU", "QBNN",
              "TimeUS,FmtType,UnitIds,MultIds");
    bytes->append(char(0xA3));
    bytes->append(char(0x95));
    bytes->append(char(kFmtuId));
    QDataStream stream(bytes, QIODevice::Append);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint64(500) << kGpsId;
    bytes->append(fixedField("s--sCCcDDmnhnhn", 16));
    bytes->append(fixedField("F--CBBBGGB000-0", 16));
}

// Real DataFlash logs open with the FMT record describing FMT itself. The
// fixtures deliberately define GPS as the only data type: the shared parsers
// defer the first descriptor until another FMT arrives, and the exporter must
// still handle such a GPS-only log.
void appendFmtHeader(QByteArray *bytes)
{
    appendFmt(bytes, 0x80, 89, "FMT", "BBnNZ",
              "Type,Length,Name,Format,Columns");
}

QByteArray binaryLog(bool withUnitData)
{
    QByteArray bytes;
    appendFmtHeader(&bytes);
    appendFmt(&bytes, kGpsId, kGpsLength, "GPS", kGpsFormat, kGpsLabels);
    if (withUnitData) {
        appendGpsFmtu(&bytes);
    }
    appendGps(&bytes, 1000, 1, 473977419, 85455938, 450);   // no fix: skipped
    appendGps(&bytes, 2000, 3, 473977419, 85455938, 450);
    appendGps(&bytes, 3000, 4, 473977429, 85455948, 470);
    appendGps(&bytes, 4000, 3, 0, 0, 470);                  // null island
    appendGps(&bytes, 5000, 3, 950000000, 85455948, 470);   // lat out of range
    return bytes;
}

const char *const kAsciiFmtHeader =
    "FMT,128,89,FMT,BBnNZ,Type,Length,Name,Format,Columns\n";
const char *const kAsciiGpsFmt =
    "FMT,150,51,GPS,QBBIHBcLLeffffB,TimeUS,I,Status,GMS,GWk,NSats,HDop,Lat,Lng,Alt,Spd,GCrs,VZ,Yaw,U\n";
const char *const kAsciiDummyFmt = "FMT,152,11,DUMY,Q,TimeUS\n";

QByteArray asciiLog()
{
    // GPS-only ASCII log, no further FMT record after GPS.
    QByteArray log(kAsciiFmtHeader);
    log += kAsciiGpsFmt;
    log += "GPS,1000,0,1,0,0,10,1.2,47.3977419,8.5455938,4.5,0,0,0,0,1\n";
    log += "GPS,2000,0,3,0,0,10,1.2,47.3977419,8.5455938,4.5,0,0,0,0,1\n";
    log += "GPS,3000,0,4,0,0,10,1.2,47.3977429,8.5455948,4.7,0,0,0,0,1\n";
    return log;
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(contents) == contents.size() && file.flush();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

struct ParsedKml
{
    QStringList path;            // element names in document order
    QString folderName;
    QString placemarkName;
    QString extrude;
    QString altitudeMode;
    QString coordinates;
    bool valid = false;
};

ParsedKml parseKml(const QByteArray &bytes)
{
    ParsedKml parsed;
    QXmlStreamReader reader(bytes);
    QStringList stack;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            stack.append(reader.name().toString());
            parsed.path.append(stack.join(QLatin1Char('/')));
            const QString current = stack.join(QLatin1Char('/'));
            if (current == QLatin1String("kml/Document/Folder/name")) {
                parsed.folderName = reader.readElementText();
                stack.removeLast();
            } else if (current
                       == QLatin1String(
                           "kml/Document/Folder/Placemark/name")) {
                parsed.placemarkName = reader.readElementText();
                stack.removeLast();
            } else if (current
                       == QLatin1String("kml/Document/Folder/Placemark/"
                                        "LineString/extrude")) {
                parsed.extrude = reader.readElementText();
                stack.removeLast();
            } else if (current
                       == QLatin1String("kml/Document/Folder/Placemark/"
                                        "LineString/altitudeMode")) {
                parsed.altitudeMode = reader.readElementText();
                stack.removeLast();
            } else if (current
                       == QLatin1String("kml/Document/Folder/Placemark/"
                                        "LineString/coordinates")) {
                parsed.coordinates = reader.readElementText();
                stack.removeLast();
            }
        } else if (reader.isEndElement() && !stack.isEmpty()) {
            stack.removeLast();
        }
    }
    parsed.valid = !reader.hasError();
    return parsed;
}

const QString kExpectedCoordinates = QStringLiteral(
    "8.5455938,47.3977419,4.5 8.5455948,47.3977429,4.7");

} // namespace

class DataFlashKmlExporterTest final : public QObject
{
    Q_OBJECT

private slots:
    void binaryTrackMatchesReferenceLayout();
    void binaryLogWithUnitDataKeepsReferenceScaling();
    void asciiLogUsesEngineeringUnits();
    void filtersNoFixNonFiniteAndOutOfRange();
    void emptyTrackIsStillValidKml();
    void unicodePathsAndErrorsPreserveOldOutput();
    void cancellationPreservesOldOutput();
    void refusesToReplaceSourceLog();
};

void DataFlashKmlExporterTest::refusesToReplaceSourceLog()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("source.bin"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray original("original source log");
    QCOMPARE(file.write(original), qint64(original.size()));
    file.close();
    const auto result = DataFlashKmlExporter::Export(path, path);
    QVERIFY(!result.succeeded);
    QVERIFY(result.error.contains(QStringLiteral("source log")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);
}

void DataFlashKmlExporterTest::binaryTrackMatchesReferenceLayout()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.bin"));
    const QString output = directory.filePath(QStringLiteral("flight.kml"));
    QVERIFY(writeFile(input, binaryLog(false)));

    const DataFlashKmlExporter::Result result =
        DataFlashKmlExporter::Export(input, output);
    QVERIFY2(result.succeeded, qPrintable(result.error));
    QVERIFY(!result.cancelled);
    QVERIFY(result.error.isEmpty());
    QCOMPARE(result.pointCount, quint64(2));

    const QByteArray kml = readFile(output);
    QVERIFY(kml.startsWith("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"));
    const ParsedKml parsed = parseKml(kml);
    QVERIFY(parsed.valid);
    QCOMPARE(parsed.path,
             QStringList({QStringLiteral("kml"),
                          QStringLiteral("kml/Document"),
                          QStringLiteral("kml/Document/Folder"),
                          QStringLiteral("kml/Document/Folder/name"),
                          QStringLiteral("kml/Document/Folder/Placemark"),
                          QStringLiteral("kml/Document/Folder/Placemark/name"),
                          QStringLiteral(
                              "kml/Document/Folder/Placemark/LineString"),
                          QStringLiteral(
                              "kml/Document/Folder/Placemark/LineString/extrude"),
                          QStringLiteral("kml/Document/Folder/Placemark/"
                                         "LineString/altitudeMode"),
                          QStringLiteral("kml/Document/Folder/Placemark/"
                                         "LineString/coordinates")}));
    QCOMPARE(parsed.folderName, QStringLiteral("Track"));
    QCOMPARE(parsed.placemarkName, QStringLiteral("Flight Path"));
    QCOMPARE(parsed.extrude, QStringLiteral("1"));
    QCOMPARE(parsed.altitudeMode, QStringLiteral("absolute"));
    QCOMPARE(parsed.coordinates.trimmed(), kExpectedCoordinates);
}

void DataFlashKmlExporterTest::binaryLogWithUnitDataKeepsReferenceScaling()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("modern.bin"));
    const QString output = directory.filePath(QStringLiteral("modern.kml"));
    QVERIFY(writeFile(input, binaryLog(true)));

    // With FMTU data the parser hands raw integers through; the exporter
    // must still apply the reference format-character scaling.
    const DataFlashKmlExporter::Result result =
        DataFlashKmlExporter::Export(input, output);
    QVERIFY2(result.succeeded, qPrintable(result.error));
    QCOMPARE(result.pointCount, quint64(2));
    const ParsedKml parsed = parseKml(readFile(output));
    QVERIFY(parsed.valid);
    QCOMPARE(parsed.coordinates.trimmed(), kExpectedCoordinates);
}

void DataFlashKmlExporterTest::asciiLogUsesEngineeringUnits()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.log"));
    const QString output = directory.filePath(QStringLiteral("flight.kml"));
    QVERIFY(writeFile(input, asciiLog()));

    const DataFlashKmlExporter::Result result =
        DataFlashKmlExporter::Export(input, output);
    QVERIFY2(result.succeeded, qPrintable(result.error));
    QCOMPARE(result.pointCount, quint64(2));
    const ParsedKml parsed = parseKml(readFile(output));
    QVERIFY(parsed.valid);
    QCOMPARE(parsed.coordinates.trimmed(), kExpectedCoordinates);
}

void DataFlashKmlExporterTest::filtersNoFixNonFiniteAndOutOfRange()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("filter.log"));
    const QString output = directory.filePath(QStringLiteral("filter.kml"));
    QByteArray log(kAsciiFmtHeader);
    log += kAsciiGpsFmt;
    log += "GPS,1000,0,2,0,0,10,1.2,47.3977419,8.5455938,4.5,0,0,0,0,1\n";   // status 2
    log += "GPS,2000,0,3,0,0,10,1.2,nan,8.5455938,4.5,0,0,0,0,1\n";          // NaN lat
    log += "GPS,3000,0,3,0,0,10,1.2,47.3977419,8.5455938,inf,0,0,0,0,1\n";   // inf alt
    log += "GPS,4000,0,3,0,0,10,1.2,95.0,8.5455938,4.5,0,0,0,0,1\n";         // lat 95
    log += "GPS,5000,0,3,0,0,10,1.2,47.3977419,181.0,4.5,0,0,0,0,1\n";       // lng 181
    log += "GPS,6000,0,3,0,0,10,1.2,0,0,4.5,0,0,0,0,1\n";                    // 0,0
    log += "GPS,7000,0,3,0,0,10,1.2,-90,180,-12.5,0,0,0,0,1\n";              // boundary kept
    QVERIFY(writeFile(input, log));

    const DataFlashKmlExporter::Result result =
        DataFlashKmlExporter::Export(input, output);
    QVERIFY2(result.succeeded, qPrintable(result.error));
    QCOMPARE(result.pointCount, quint64(1));
    const ParsedKml parsed = parseKml(readFile(output));
    QVERIFY(parsed.valid);
    QCOMPARE(parsed.coordinates.trimmed(), QStringLiteral("180,-90,-12.5"));
}

void DataFlashKmlExporterTest::emptyTrackIsStillValidKml()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("nogps.log"));
    const QString output = directory.filePath(QStringLiteral("nogps.kml"));
    // GPS is defined but never logged; a second type with a row proves that
    // non-GPS records are ignored and a valid empty track is still written.
    QByteArray log(kAsciiFmtHeader);
    log += kAsciiGpsFmt;
    log += kAsciiDummyFmt;
    log += "DUMY,1000\n";
    QVERIFY(writeFile(input, log));

    const DataFlashKmlExporter::Result result =
        DataFlashKmlExporter::Export(input, output);
    QVERIFY2(result.succeeded, qPrintable(result.error));
    QCOMPARE(result.pointCount, quint64(0));
    const ParsedKml parsed = parseKml(readFile(output));
    QVERIFY(parsed.valid);
    QCOMPARE(parsed.placemarkName, QStringLiteral("Flight Path"));
    QVERIFY(parsed.coordinates.trimmed().isEmpty());
}

void DataFlashKmlExporterTest::unicodePathsAndErrorsPreserveOldOutput()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString folder = directory.filePath(QStringLiteral("Полёт №7"));
    QVERIFY(QDir().mkpath(folder));
    const QString input = folder + QStringLiteral("/журнал.bin");
    const QString output = folder + QStringLiteral("/трек.kml");
    const QByteArray oldContent("OLD KML MUST SURVIVE");
    QVERIFY(writeFile(output, oldContent));

    // Missing input: error, old output untouched.
    DataFlashKmlExporter::Result result = DataFlashKmlExporter::Export(
        folder + QStringLiteral("/нет.bin"), output);
    QVERIFY(!result.succeeded);
    QVERIFY(!result.cancelled);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(result.pointCount, quint64(0));
    QCOMPARE(readFile(output), oldContent);

    // Garbage input without any FMT record: error, old output untouched.
    QVERIFY(writeFile(input, QByteArray(4096, 'x')));
    result = DataFlashKmlExporter::Export(input, output);
    QVERIFY(!result.succeeded);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(readFile(output), oldContent);

    // Valid Unicode input path replaces the old output atomically.
    QVERIFY(writeFile(input, binaryLog(false)));
    result = DataFlashKmlExporter::Export(input, output);
    QVERIFY2(result.succeeded, qPrintable(result.error));
    QCOMPARE(result.pointCount, quint64(2));
    const ParsedKml parsed = parseKml(readFile(output));
    QVERIFY(parsed.valid);
    QCOMPARE(parsed.coordinates.trimmed(), kExpectedCoordinates);
    QVERIFY(QDir(folder).entryList(QDir::Files).size() == 2);
}

void DataFlashKmlExporterTest::cancellationPreservesOldOutput()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("cancel.bin"));
    const QString output = directory.filePath(QStringLiteral("cancel.kml"));
    const QByteArray oldContent("OLD KML MUST SURVIVE");
    QVERIFY(writeFile(input, binaryLog(false)));
    QVERIFY(writeFile(output, oldContent));

    // Cancelled before the first record.
    DataFlashKmlExporter::Result result = DataFlashKmlExporter::Export(
        input, output, []() { return true; });
    QVERIFY(!result.succeeded);
    QVERIFY(result.cancelled);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(readFile(output), oldContent);

    // Cancelled after the first accepted record has already been written.
    int polls = 0;
    result = DataFlashKmlExporter::Export(input, output, [&polls]() {
        return ++polls > 3;
    });
    QVERIFY(!result.succeeded);
    QVERIFY(result.cancelled);
    QCOMPARE(readFile(output), oldContent);
    QVERIFY(QDir(directory.path()).entryList(QDir::Files).size() == 2);

    // Never cancelled: the export goes through.
    result = DataFlashKmlExporter::Export(input, output, []() { return false; });
    QVERIFY2(result.succeeded, qPrintable(result.error));
    QCOMPARE(result.pointCount, quint64(2));
}

QTEST_GUILESS_MAIN(DataFlashKmlExporterTest)

#include "test_dataflashkmlexporter.moc"
