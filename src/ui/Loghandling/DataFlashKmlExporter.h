#ifndef DATAFLASHKMLEXPORTER_H
#define DATAFLASHKMLEXPORTER_H

#include <QString>
#include <QtGlobal>

#include <functional>

/**
 * Standalone DataFlash (.bin / .log) to KML track exporter.
 *
 * Mirrors MP10 Services/DataFlashLog.ExportKml: only GPS records (not GPS2)
 * with Status >= 3, finite Lat/Lng/Alt, Lat within [-90, 90], Lng within
 * [-180, 180] and not both zero are written, in log order, as one
 * Folder "Track" / Placemark "Flight Path" / LineString (extrude 1,
 * altitudeMode absolute) with "lng,lat,alt" coordinates.
 *
 * The exporter streams: records flow from BinLogParser/AsciiLogParser through
 * a private ILogdataSink straight into a QXmlStreamWriter on a QSaveFile.
 * Nothing is buffered in a LogdataStorage, no GUI or processEvents is
 * involved, and the caller may run Export() on a worker thread.
 *
 * Atomicity: the output path is replaced only when the export succeeded. On
 * error or cancellation the previous file at the output path, if any, is left
 * untouched. Unicode paths are handled through QString/QFile.
 */
class DataFlashKmlExporter
{
public:
    /** Returns true when the caller wants the export to stop. */
    using CancellationCheck = std::function<bool()>;

    struct Result
    {
        bool succeeded = false;
        bool cancelled = false;
        QString error;
        quint64 pointCount = 0;
    };

    /**
     * Exports the GPS track of @p input (a .bin or ASCII .log DataFlash file)
     * to @p output as KML.
     *
     * An empty track is still a successful export (pointCount 0) and produces
     * a valid KML document with an empty LineString, as the reference does.
     * @p isCancelled is polled between records; when it returns true the
     * export stops, Result::cancelled is set and @p output is not touched.
     */
    static Result Export(const QString &input, const QString &output,
                         const CancellationCheck &isCancelled = {});

private:
    DataFlashKmlExporter() = delete;
};

#endif // DATAFLASHKMLEXPORTER_H
