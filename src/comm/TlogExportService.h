#ifndef TLOGEXPORTSERVICE_H
#define TLOGEXPORTSERVICE_H

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

#include <mavlink.h>

#include <functional>

class TlogReader;

/*
 * Mission Planner 10 "Tlog Convert / Extract" core (Services/TlogExportService.cs,
 * ViewModels/MavlinkLogConvertViewModel.cs, Services/DataFlashLog.cs track writers),
 * without UI, LinkManager or the Matlab writer.
 *
 * Every export streams the .tlog through TlogReader and writes its result with
 * QSaveFile, so each output file is replaced atomically and cancellation never
 * commits a partially written file. Mission snapshots are all staged before
 * the commit phase; filesystem commit failures can still occur between files.
 *
 * MP10 semantics reproduced:
 * - KML/GPX: GLOBAL_POSITION_INT positions (lat/1e7, lon/1e7, alt/1000 m),
 *   dropping |lat| > 90, |lon| > 180 and (0, 0); KML LineString "Flight Path"
 *   (absolute altitude, extruded); GPX trk/trkseg/trkpt with ele and time.
 * - CSV / Text: one line per decoded packet of the bundled dialect:
 *   timestamp (CSV: UTC "yyyy-MM-ddTHH:mm:ss.fffZ"; Text: ISO round-trip
 *   "yyyy-MM-ddTHH:mm:ss.fffffffZ") + delimiter + MESSAGE_NAME + fields as
 *   name=value joined by the delimiter ("," or " ").
 * - Parameters: latest PARAM_VALUE per name (case-insensitive), sorted
 *   case-insensitively, lines "NAME<TAB>value" (C# G17 -> 17 significant digits).
 *   Senders that announced MAV_AUTOPILOT_ARDUPILOTMEGA in a HEARTBEAT carry the
 *   numeric value in the float (C-style), all others are decoded bytewise by
 *   the declared MAV_PARAM_TYPE (ParameterCodec).
 * - Missions: per (sysid, compid) MISSION_COUNT (MAV_MISSION_TYPE_MISSION,
 *   count > 0) opens a builder; MISSION_ITEM and MISSION_ITEM_INT arrive in any
 *   order (INT x/y divided by 1e7 for global frames); a snapshot completes when
 *   every sequence is present, duplicates are dropped by a full-field signature,
 *   each snapshot is a "QGC WPL 110" file; the first uses the selected path,
 *   the others get "-2", "-3", ... before the extension.
 */
enum class TlogExportFormat { Kml, Gpx, Csv, Text, Parameters, Missions };

struct TlogExportResult
{
    bool success = false;
    bool cancelled = false;
    int itemCount = 0;          // packets, parameters, snapshots or track points written
    QStringList outputPaths;    // committed files; may be nonempty if a later mission-file commit fails
    QString error;              // MP10 error text when !success && !cancelled
    // Additional detail (not required by the window):
    QString message;            // MP10 success text, e.g. "Wrote 512 decoded packets to /x.csv"
    qint64 recordsRead = 0;     // frames decoded from the tlog
    qint64 skippedBytes = 0;    // bytes discarded while resyncing
};

struct TlogTrackPoint
{
    double latitude = 0.0;      // degrees
    double longitude = 0.0;     // degrees
    double altitudeMeters = 0.0;// GLOBAL_POSITION_INT alt (AMSL) in metres
    qint64 timestampUsec = 0;   // tlog timestamp
};

struct TlogMissionItem
{
    quint16 sequence = 0;
    quint8 current = 0;
    quint8 frame = 0;
    quint16 command = 0;
    float param1 = 0.0f;
    float param2 = 0.0f;
    float param3 = 0.0f;
    float param4 = 0.0f;
    double x = 0.0;             // degrees for global frames; metres for local frames
    double y = 0.0;
    double z = 0.0;
    quint8 autoContinue = 0;
};

struct TlogParameter
{
    QString name;               // first-seen spelling
    double value = 0.0;         // latest value
};

class TlogExportService
{
public:
    using CancelRequested = std::function<bool()>;
    using Progress = std::function<void(qint64 bytesProcessed, qint64 bytesTotal)>;

    // Integration entry point (MavlinkLogWindow). selectedOutput is the file the
    // user chose; Missions may add "-2", "-3", ... siblings.
    static TlogExportResult Export(TlogExportFormat format, const QString &input,
                                   const QString &selectedOutput,
                                   const CancelRequested &cancel = {});
    static TlogExportResult Export(TlogExportFormat format, const QString &input,
                                   const QString &selectedOutput,
                                   const CancelRequested &cancel, const Progress &progress);

    // --- MP10 texts ---
    static QString FormatLabel(TlogExportFormat format);      // "KML", "GPX", "CSV", "human-readable text", "parameters", "mission snapshots"
    static QString DefaultExtension(TlogExportFormat format); // "kml", "gpx", "csv", "txt", "param", "waypoints"
    static QString NoGpsPositionsText();  // "No GPS positions found in the tlog."
    static QString NoParametersText();    // "No PARAM_VALUE messages were found in the tlog."
    static QString NoMissionText();       // "No complete mission transfer was found in the tlog."
    static QString CancelledText(TlogExportFormat format);    // "<label> export cancelled."

    // --- pure building blocks (also used by the tests) ---
    static QVector<TlogTrackPoint> ReadTrack(TlogReader &reader,
                                             const CancelRequested &cancel = {});
    // Key = upper-cased name; value keeps the first-seen spelling and the latest value.
    static QMap<QString, TlogParameter> ExtractParameters(TlogReader &reader,
                                                          const CancelRequested &cancel = {});
    static QVector<QVector<TlogMissionItem>> ExtractMissionSnapshots(
        TlogReader &reader, const CancelRequested &cancel = {});

    static bool IsValidTrackPosition(double latitude, double longitude);
    static bool IsGlobalFrame(quint8 frame);   // GLOBAL, GLOBAL_RELATIVE_ALT, GLOBAL_TERRAIN_ALT and their INT forms
    static QString DescribePacket(const mavlink_message_t &message, const QString &delimiter);
    static QString FormatCsvTimestamp(qint64 timestampUsec);   // yyyy-MM-ddTHH:mm:ss.fffZ
    static QString FormatIsoTimestamp(qint64 timestampUsec);   // yyyy-MM-ddTHH:mm:ss.fffffffZ
    static QString FormatParameterValue(double value);          // C# G17
    static QString QgcWplLine(const TlogMissionItem &item);
    static QString MissionSignature(const TlogMissionItem &item);
    static QStringList SnapshotOutputPaths(const QString &selectedOutput, int count);
    static QString KmlDocument(const QVector<TlogTrackPoint> &track);
    static QString GpxDocument(const QVector<TlogTrackPoint> &track);
    static QStringList ParameterLines(const QMap<QString, TlogParameter> &parameters);
};

#endif // TLOGEXPORTSERVICE_H
