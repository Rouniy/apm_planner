#include "DataFlashKmlExporter.h"

#include "AsciiLogParser.h"
#include "BinLogParser.h"
#include "ILogParser.h"
#include "ILogdataSink.h"
#include "IParserCallback.h"

#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLocale>
#include <QSaveFile>
#include <QScopedPointer>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QXmlStreamWriter>
#include <QtGlobal>

#include <cmath>

namespace {

const QString kGpsType = QStringLiteral("GPS");
const QString kStatusLabel = QStringLiteral("Status");
const QString kLatLabel = QStringLiteral("Lat");
const QString kLngLabel = QStringLiteral("Lng");
const QString kAltLabel = QStringLiteral("Alt");
const int kMinimumFixStatus = 3;

bool cancellationRequested(
    const DataFlashKmlExporter::CancellationCheck &check)
{
    return check && check();
}

/**
 * Shortest round-trip decimal with a '.' separator, the same shape the
 * reference produces through invariant-culture double formatting.
 */
QString formatCoordinate(double value)
{
    return QString::number(value, 'g', QLocale::FloatingPointShortest);
}

/**
 * Divisor the reference applies purely from the FMT format character.
 * BinLogParser divides by the same values itself while a log carries no
 * FMTU unit data and hands raw integers through once it does; the sink
 * mirrors that switch so every log ends up in reference units. Dividing
 * (not multiplying by a reciprocal) keeps the result bit-identical to the
 * parser's own scaling.
 */
double referenceDivisor(QChar formatChar)
{
    switch (formatChar.toLatin1()) {
    case 'L':
        return 10000000.0;
    case 'c':
    case 'C':
    case 'e':
    case 'E':
        return 100.0;
    default:
        return 1.0;
    }
}

/** Streams accepted GPS fixes straight into the open coordinates element. */
class KmlTrackSink final : public ILogdataSink
{
public:
    KmlTrackSink(QXmlStreamWriter *writer, bool binaryLog,
                 const DataFlashKmlExporter::CancellationCheck &isCancelled)
        : m_writer(writer)
        , m_binaryLog(binaryLog)
        , m_isCancelled(isCancelled)
    {
    }

    bool addDataType(const QString &typeName, quint32, int,
                     const QString &typeFormat, const QStringList &typeLabels,
                     int) override
    {
        m_sawAnyType = true;
        if (typeName == kGpsType) {
            m_gpsFormat = typeFormat;
            m_gpsLabels = typeLabels;
        }
        return true;
    }

    bool addDataRow(const QString &typeName,
                    const QList<QPair<QString, QVariant>> &values) override
    {
        if (cancellationRequested(m_isCancelled)) {
            m_cancelled = true;
            m_error = QStringLiteral("KML export was cancelled.");
            return false;
        }
        if (typeName != kGpsType) {
            return true;
        }

        int status = 0;
        double lat = 0.0;
        double lng = 0.0;
        double alt = 0.0;
        if (!readInt(values, kStatusLabel, &status)
            || status < kMinimumFixStatus) {
            return true;
        }
        if (!readScaled(values, kLatLabel, &lat)
            || !readScaled(values, kLngLabel, &lng)
            || !readScaled(values, kAltLabel, &alt)) {
            return true;
        }
        // The reference range test cannot reject NaN; a coordinate that is
        // not a finite number is dropped here instead of written verbatim.
        if (!std::isfinite(lat) || !std::isfinite(lng)
            || !std::isfinite(alt)) {
            return true;
        }
        if (lat < -90.0 || lat > 90.0 || lng < -180.0 || lng > 180.0) {
            return true;
        }
        if (lat == 0.0 && lng == 0.0) {
            return true;
        }

        if (m_pointCount > 0) {
            m_writer->writeCharacters(QStringLiteral(" "));
        }
        m_writer->writeCharacters(QStringLiteral("%1,%2,%3")
                                      .arg(formatCoordinate(lng),
                                           formatCoordinate(lat),
                                           formatCoordinate(alt)));
        if (m_writer->hasError()) {
            m_error = QStringLiteral("Writing the KML track failed.");
            return false;
        }
        ++m_pointCount;
        return true;
    }

    void addUnitData(quint8, const QString &) override {}
    void addMultiplierData(quint8, double) override {}
    void addMsgToUnitAndMultiplierData(quint32, const QByteArray &,
                                       const QByteArray &) override
    {
        // LogParserBase flips to raw integers at exactly this call.
        m_hasUnitData = true;
    }
    void setTimeStamp(const QString &, double) override {}
    QStringList setupUnitData(const QString &, double) override
    {
        return {};
    }
    QString getError() const override { return m_error; }

    bool cancelled() const { return m_cancelled; }
    bool sawAnyType() const { return m_sawAnyType; }
    quint64 pointCount() const { return m_pointCount; }

private:
    static bool readInt(const QList<QPair<QString, QVariant>> &values,
                        const QString &label, int *result)
    {
        for (const auto &pair : values) {
            if (pair.first == label) {
                bool ok = false;
                const int value = pair.second.toInt(&ok);
                if (ok) {
                    *result = value;
                }
                return ok;
            }
        }
        return false;
    }

    bool readScaled(const QList<QPair<QString, QVariant>> &values,
                    const QString &label, double *result) const
    {
        for (const auto &pair : values) {
            if (pair.first != label) {
                continue;
            }
            bool ok = false;
            double value = pair.second.toDouble(&ok);
            if (!ok) {
                return false;
            }
            if (m_binaryLog && m_hasUnitData) {
                const int index = m_gpsLabels.indexOf(label);
                if (index >= 0 && index < m_gpsFormat.size()) {
                    value /= referenceDivisor(m_gpsFormat.at(index));
                }
            }
            *result = value;
            return true;
        }
        return false;
    }

    QXmlStreamWriter *m_writer;
    bool m_binaryLog;
    DataFlashKmlExporter::CancellationCheck m_isCancelled;
    QString m_gpsFormat;
    QStringList m_gpsLabels;
    QString m_error;
    bool m_hasUnitData = false;
    bool m_cancelled = false;
    bool m_sawAnyType = false;
    quint64 m_pointCount = 0;
};

class ExportParserCallback final : public IParserCallback
{
public:
    explicit ExportParserCallback(
        const DataFlashKmlExporter::CancellationCheck &isCancelled)
        : m_isCancelled(isCancelled)
    {
    }

    void setParser(ILogParser *parser) { m_parser = parser; }

    void onProgress(qint64, qint64) override
    {
        if (cancellationRequested(m_isCancelled) && m_parser) {
            m_cancelled = true;
            m_parser->stopParsing();
        }
    }

    void onError(const QString &errorMessage) override
    {
        m_error = errorMessage;
        if (m_parser) {
            m_parser->stopParsing();
        }
    }

    bool cancelled() const { return m_cancelled; }
    QString error() const { return m_error; }

private:
    DataFlashKmlExporter::CancellationCheck m_isCancelled;
    ILogParser *m_parser = nullptr;
    QString m_error;
    bool m_cancelled = false;
};

/**
 * LogParserBase stores a type descriptor only once a timestamp type is
 * known and even then defers the descriptor that established it until the
 * next FMT record arrives. Real logs define dozens of types, so this never
 * shows; a log whose only defined type is GPS (or any single-FMT log) would
 * silently drop every record. These exporter-private parsers pre-select the
 * ArduPilot TimeUS timestamp so the very first descriptor is stored at once.
 * Types without a TimeUS label are then handled by the parser's own
 * "no timestamp" path: the parser keeps the original format for decoding,
 * prepends a synthetic TimeUS column to the labels/format it reports and to
 * each row, and Lat/Lng/Alt/Status are still looked up by label, so the KML
 * output is unaffected (time is not exported).
 */
class KmlBinLogParser final : public BinLogParser
{
public:
    using BinLogParser::BinLogParser;

    void presetTimestamp()
    {
        m_activeTimestamp = timeStampType(QStringLiteral("TimeUS"), 1000000.0);
    }
};

class KmlAsciiLogParser final : public AsciiLogParser
{
public:
    using AsciiLogParser::AsciiLogParser;

    void presetTimestamp()
    {
        m_activeTimestamp = timeStampType(QStringLiteral("TimeUS"), 1000000.0);
    }
};

DataFlashKmlExporter::Result failure(const QString &error)
{
    DataFlashKmlExporter::Result result;
    result.error = error;
    return result;
}

DataFlashKmlExporter::Result cancelledResult()
{
    DataFlashKmlExporter::Result result;
    result.cancelled = true;
    result.error = QStringLiteral("KML export was cancelled.");
    return result;
}

bool looksBinary(const QFileInfo &info, QFile *file)
{
    const QString suffix = info.suffix();
    if (suffix.compare(QStringLiteral("bin"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    if (suffix.compare(QStringLiteral("log"), Qt::CaseInsensitive) == 0) {
        return false;
    }
    // Unknown suffix: sniff the DataFlash frame header 0xA3 0x95.
    const QByteArray head = file->peek(2);
    return head.size() == 2 && static_cast<quint8>(head.at(0)) == 0xA3
        && static_cast<quint8>(head.at(1)) == 0x95;
}

} // namespace

DataFlashKmlExporter::Result DataFlashKmlExporter::Export(
    const QString &input, const QString &output,
    const CancellationCheck &isCancelled)
{
    if (cancellationRequested(isCancelled)) {
        return cancelledResult();
    }
    if (output.trimmed().isEmpty()) {
        return failure(QStringLiteral("No KML output path was given."));
    }

    const QFileInfo info(input);
    const QFileInfo outputInfo(output);
    if (info.absoluteFilePath() == outputInfo.absoluteFilePath()
        || (!info.canonicalFilePath().isEmpty()
            && info.canonicalFilePath() == outputInfo.canonicalFilePath())) {
        return failure(QStringLiteral("The KML output must not replace the source log."));
    }
    if (!info.exists() || !info.isFile()) {
        return failure(QStringLiteral("The DataFlash log does not exist: %1")
                           .arg(input));
    }
    QFile logFile(info.absoluteFilePath());
    if (!logFile.open(QIODevice::ReadOnly)) {
        return failure(QStringLiteral("Cannot read the DataFlash log %1: %2")
                           .arg(input, logFile.errorString()));
    }
    const bool binaryLog = looksBinary(info, &logFile);

    // QSaveFile writes to a temporary sibling and replaces the target only
    // on commit(); every other exit leaves an existing output untouched.
    QSaveFile saveFile(output);
    saveFile.setDirectWriteFallback(false);
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return failure(QStringLiteral("Cannot write the KML file %1: %2")
                           .arg(output, saveFile.errorString()));
    }

    QXmlStreamWriter xml(&saveFile);
    xml.setAutoFormatting(true);
    xml.setAutoFormattingIndent(2);
    xml.writeStartDocument(QStringLiteral("1.0"));
    // Same element tree as the reference KMLRoot serializer: kml, Document,
    // Folder "Track", Placemark "Flight Path", LineString with extrude 1 and
    // absolute altitudes, coordinates as "lng,lat,alt" separated by spaces.
    xml.writeStartElement(QStringLiteral("kml"));
    xml.writeStartElement(QStringLiteral("Document"));
    xml.writeStartElement(QStringLiteral("Folder"));
    xml.writeTextElement(QStringLiteral("name"), QStringLiteral("Track"));
    xml.writeStartElement(QStringLiteral("Placemark"));
    xml.writeTextElement(QStringLiteral("name"), QStringLiteral("Flight Path"));
    xml.writeStartElement(QStringLiteral("LineString"));
    xml.writeTextElement(QStringLiteral("extrude"), QStringLiteral("1"));
    xml.writeTextElement(QStringLiteral("altitudeMode"),
                         QStringLiteral("absolute"));
    xml.writeStartElement(QStringLiteral("coordinates"));

    QSharedPointer<KmlTrackSink> sink(
        new KmlTrackSink(&xml, binaryLog, isCancelled));
    ExportParserCallback callback(isCancelled);
    QScopedPointer<ILogParser> parser;
    if (binaryLog) {
        auto *binaryParser = new KmlBinLogParser(
            qSharedPointerCast<ILogdataSink>(sink), &callback);
        binaryParser->presetTimestamp();
        parser.reset(binaryParser);
    } else {
        auto *asciiParser = new KmlAsciiLogParser(
            qSharedPointerCast<ILogdataSink>(sink), &callback);
        asciiParser->presetTimestamp();
        parser.reset(asciiParser);
    }
    callback.setParser(parser.data());

    const AP2DataPlotStatus status = parser->parse(logFile);
    Q_UNUSED(status)

    if (sink->cancelled() || callback.cancelled()
        || cancellationRequested(isCancelled)) {
        saveFile.cancelWriting();
        return cancelledResult();
    }
    if (!callback.error().isEmpty()) {
        saveFile.cancelWriting();
        return failure(QStringLiteral("Parsing the DataFlash log failed: %1")
                           .arg(callback.error()));
    }
    if (!sink->getError().isEmpty()) {
        saveFile.cancelWriting();
        return failure(sink->getError());
    }
    if (!sink->sawAnyType()) {
        saveFile.cancelWriting();
        return failure(QStringLiteral(
            "%1 is not a DataFlash log: no FMT record was found.")
                           .arg(input));
    }

    xml.writeEndElement(); // coordinates
    xml.writeEndElement(); // LineString
    xml.writeEndElement(); // Placemark
    xml.writeEndElement(); // Folder
    xml.writeEndElement(); // Document
    xml.writeEndElement(); // kml
    xml.writeEndDocument();

    if (xml.hasError() || saveFile.error() != QFileDevice::NoError) {
        const QString detail = saveFile.errorString();
        saveFile.cancelWriting();
        return failure(QStringLiteral("Writing the KML file %1 failed: %2")
                           .arg(output, detail));
    }
    if (cancellationRequested(isCancelled)) {
        saveFile.cancelWriting();
        return cancelledResult();
    }
    if (!saveFile.commit()) {
        return failure(QStringLiteral("Writing the KML file %1 failed: %2")
                           .arg(output, saveFile.errorString()));
    }

    Result result;
    result.succeeded = true;
    result.pointCount = sink->pointCount();
    return result;
}
