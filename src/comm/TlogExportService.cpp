#include "TlogExportService.h"

#include "TlogReader.h"
#include "core/parameters/ParameterCodec.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>
#include <QXmlStreamWriter>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace {

constexpr qint64 kWriteChunkBytes = 64 * 1024;

QString comparablePath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath()
                                               : canonical);
}

const QHash<quint32, mavlink_message_info_t> &messageInfoTable()
{
    static const QHash<quint32, mavlink_message_info_t> table = []() {
        QHash<quint32, mavlink_message_info_t> result;
        static const mavlink_message_info_t infos[] = MAVLINK_MESSAGE_INFO;
        for (const mavlink_message_info_t &info : infos) {
            if (info.name && info.name[0] != '\0') {
                result.insert(info.msgid, info);
            }
        }
        return result;
    }();
    return table;
}

template<typename T>
T readPayload(const mavlink_message_t &message, unsigned int offset)
{
    T value{};
    const unsigned int payloadLength = static_cast<unsigned int>(message.len);
    if (offset < payloadLength) {
        const size_t available = qMin(sizeof(T),
                                      static_cast<size_t>(payloadLength - offset));
        std::memcpy(&value, _MAV_PAYLOAD(&message) + offset, available);
    }
    return value; // MAVLink v2 zero-truncation: missing bytes read as zero
}

QString formatScalar(const mavlink_message_t &message, mavlink_message_type_t type,
                     unsigned int offset)
{
    switch (type) {
    case MAVLINK_TYPE_CHAR: return QString(QChar::fromLatin1(readPayload<char>(message, offset)));
    case MAVLINK_TYPE_UINT8_T: return QString::number(readPayload<quint8>(message, offset));
    case MAVLINK_TYPE_INT8_T: return QString::number(readPayload<qint8>(message, offset));
    case MAVLINK_TYPE_UINT16_T: return QString::number(readPayload<quint16>(message, offset));
    case MAVLINK_TYPE_INT16_T: return QString::number(readPayload<qint16>(message, offset));
    case MAVLINK_TYPE_UINT32_T: return QString::number(readPayload<quint32>(message, offset));
    case MAVLINK_TYPE_INT32_T: return QString::number(readPayload<qint32>(message, offset));
    case MAVLINK_TYPE_UINT64_T: return QString::number(readPayload<quint64>(message, offset));
    case MAVLINK_TYPE_INT64_T: return QString::number(readPayload<qint64>(message, offset));
    case MAVLINK_TYPE_FLOAT: return QString::number(readPayload<float>(message, offset), 'g', 9);
    case MAVLINK_TYPE_DOUBLE: return QString::number(readPayload<double>(message, offset), 'g', 17);
    }
    return QString();
}

unsigned int typeSize(mavlink_message_type_t type)
{
    switch (type) {
    case MAVLINK_TYPE_CHAR:
    case MAVLINK_TYPE_UINT8_T:
    case MAVLINK_TYPE_INT8_T: return 1;
    case MAVLINK_TYPE_UINT16_T:
    case MAVLINK_TYPE_INT16_T: return 2;
    case MAVLINK_TYPE_UINT32_T:
    case MAVLINK_TYPE_INT32_T:
    case MAVLINK_TYPE_FLOAT: return 4;
    case MAVLINK_TYPE_UINT64_T:
    case MAVLINK_TYPE_INT64_T:
    case MAVLINK_TYPE_DOUBLE: return 8;
    }
    return 1;
}

QString formatField(const mavlink_message_t &message, const mavlink_field_info_t &field)
{
    if (field.array_length == 0) {
        return formatScalar(message, field.type, field.wire_offset);
    }
    if (field.type == MAVLINK_TYPE_CHAR) {
        QString text;
        for (unsigned int i = 0; i < field.array_length; ++i) {
            const char c = readPayload<char>(message, field.wire_offset + i);
            if (c == '\0') {
                break;
            }
            text.append(QChar::fromLatin1(c));
        }
        text.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QStringLiteral("\"%1\"").arg(text);
    }
    QStringList elements;
    const unsigned int size = typeSize(field.type);
    for (unsigned int i = 0; i < field.array_length; ++i) {
        elements.append(formatScalar(message, field.type, field.wire_offset + i * size));
    }
    return QStringLiteral("[%1]").arg(elements.join(QLatin1Char(';')));
}

QString parameterName(const mavlink_param_value_t &value)
{
    int length = 0;
    while (length < 16 && value.param_id[length] != '\0') {
        ++length;
    }
    return QString::fromUtf8(value.param_id, length);
}

quint16 endpointKey(const mavlink_message_t &message)
{
    return static_cast<quint16>((message.sysid << 8) | message.compid);
}

struct MissionBuilder
{
    quint16 expected = 0;
    QMap<quint16, TlogMissionItem> items;
};

TlogExportResult fail(const QString &error)
{
    TlogExportResult result;
    result.error = error;
    return result;
}

bool writeText(const QString &path, const QString &content,
               const TlogExportService::CancelRequested &cancel,
               bool *wasCancelled, QString *error)
{
    if (wasCancelled) {
        *wasCancelled = false;
    }
    if (cancel && cancel()) {
        if (wasCancelled) {
            *wasCancelled = true;
        }
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    const QByteArray bytes = content.toUtf8(); // UTF-8 without BOM, like MP10
    qint64 offset = 0;
    while (offset < bytes.size()) {
        if (cancel && cancel()) {
            file.cancelWriting();
            if (wasCancelled) {
                *wasCancelled = true;
            }
            return false;
        }
        const qint64 chunk = qMin(kWriteChunkBytes,
                                  static_cast<qint64>(bytes.size()) - offset);
        const qint64 written = file.write(bytes.constData() + offset, chunk);
        if (written != chunk) {
            file.cancelWriting();
            if (error) {
                *error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
            }
            return false;
        }
        offset += written;
    }
    if (cancel && cancel()) {
        file.cancelWriting();
        if (wasCancelled) {
            *wasCancelled = true;
        }
        return false;
    }
    if (!file.commit()) {
        if (error) {
            *error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    return true;
}

} // namespace

// --- texts ---------------------------------------------------------------------------------

QString TlogExportService::FormatLabel(TlogExportFormat format)
{
    switch (format) {
    case TlogExportFormat::Kml: return QStringLiteral("KML");
    case TlogExportFormat::Gpx: return QStringLiteral("GPX");
    case TlogExportFormat::Csv: return QStringLiteral("CSV");
    case TlogExportFormat::Text: return QStringLiteral("human-readable text");
    case TlogExportFormat::Parameters: return QStringLiteral("parameters");
    case TlogExportFormat::Missions: return QStringLiteral("mission snapshots");
    }
    return QString();
}

QString TlogExportService::DefaultExtension(TlogExportFormat format)
{
    switch (format) {
    case TlogExportFormat::Kml: return QStringLiteral("kml");
    case TlogExportFormat::Gpx: return QStringLiteral("gpx");
    case TlogExportFormat::Csv: return QStringLiteral("csv");
    case TlogExportFormat::Text: return QStringLiteral("txt");
    case TlogExportFormat::Parameters: return QStringLiteral("param");
    case TlogExportFormat::Missions: return QStringLiteral("waypoints");
    }
    return QString();
}

QString TlogExportService::NoGpsPositionsText()
{
    return QStringLiteral("No GPS positions found in the tlog.");
}

QString TlogExportService::NoParametersText()
{
    return QStringLiteral("No PARAM_VALUE messages were found in the tlog.");
}

QString TlogExportService::NoMissionText()
{
    return QStringLiteral("No complete mission transfer was found in the tlog.");
}

QString TlogExportService::CancelledText(TlogExportFormat format)
{
    return QStringLiteral("%1 export cancelled.").arg(FormatLabel(format));
}

// --- formatting helpers --------------------------------------------------------------------

bool TlogExportService::IsValidTrackPosition(double latitude, double longitude)
{
    return std::isfinite(latitude) && std::isfinite(longitude) && latitude >= -90.0
        && latitude <= 90.0 && longitude >= -180.0 && longitude <= 180.0
        && !(latitude == 0.0 && longitude == 0.0);
}

bool TlogExportService::IsGlobalFrame(quint8 frame)
{
    // MP10 TlogExportService.cs:176-183 (MAV_FRAME_GLOBAL, _RELATIVE_ALT, _TERRAIN_ALT and
    // the INT variants 5, 6, 11).
    return frame == MAV_FRAME_GLOBAL || frame == MAV_FRAME_GLOBAL_RELATIVE_ALT
        || frame == MAV_FRAME_GLOBAL_TERRAIN_ALT || frame == 5 || frame == 6 || frame == 11;
}

QString TlogExportService::DescribePacket(const mavlink_message_t &message, const QString &delimiter)
{
    if (message.msgid == MAVLINK_MSG_ID_SETUP_SIGNING) {
        // Historical/imported logs may contain provisioning traffic. Preserve row
        // identity, but never reflect, decode, or hex-dump the secret payload.
        // Offsets 8/9 are the common dialect's target fields; readPayload also
        // handles MAVLink 2 zero-truncation without touching secret_key at 10.
        return QStringList{
            QStringLiteral("SETUP_SIGNING"),
            QStringLiteral("source_system=%1").arg(message.sysid),
            QStringLiteral("source_component=%1").arg(message.compid),
            QStringLiteral("target_system=%1").arg(readPayload<quint8>(message, 8)),
            QStringLiteral("target_component=%1").arg(readPayload<quint8>(message, 9)),
            QStringLiteral("secret_key=[REDACTED]"),
            QStringLiteral("sensitive_payload=omitted")
        }.join(delimiter);
    }

    const auto it = messageInfoTable().constFind(message.msgid);
    if (it == messageInfoTable().constEnd()) {
        return QString(); // unknown to the bundled dialect: skipped like MP10's empty DebugPacket
    }
    QStringList parts;
    parts.append(QString::fromLatin1(it->name));
    for (unsigned int i = 0; i < it->num_fields && i < MAVLINK_MAX_FIELDS; ++i) {
        const mavlink_field_info_t &field = it->fields[i];
        parts.append(QStringLiteral("%1=%2").arg(QString::fromLatin1(field.name),
                                                 formatField(message, field)));
    }
    return parts.join(delimiter);
}

QString TlogExportService::FormatCsvTimestamp(qint64 timestampUsec)
{
    const qint64 msec = timestampUsec >= 0 ? timestampUsec / 1000
                                           : -((-timestampUsec + 999) / 1000);
    return QDateTime::fromMSecsSinceEpoch(msec, Qt::UTC)
               .toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz"))
        + QLatin1Char('Z');
}

QString TlogExportService::FormatIsoTimestamp(qint64 timestampUsec)
{
    // C# DateTime "O": yyyy-MM-ddTHH:mm:ss.fffffffZ (100 ns ticks; the tlog has microseconds).
    qint64 seconds = timestampUsec / 1000000;
    qint64 fraction = timestampUsec % 1000000;
    if (fraction < 0) {
        fraction += 1000000;
        --seconds;
    }
    return QDateTime::fromMSecsSinceEpoch(seconds * 1000, Qt::UTC)
               .toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))
        + QStringLiteral(".%1%2Z").arg(fraction, 6, 10, QLatin1Char('0')).arg(0);
}

QString TlogExportService::FormatParameterValue(double value)
{
    return QString::number(value, 'g', 17); // C# G17
}

QString TlogExportService::QgcWplLine(const TlogMissionItem &item)
{
    // MP10 WriteQgcWpl: tab separated seq, current, frame, command, p1..p4 (0.000000),
    // x, y (0.0000000), z (0.000000), autocontinue.
    return QStringList{
        QString::number(item.sequence), QString::number(item.current),
        QString::number(item.frame), QString::number(item.command),
        QString::number(item.param1, 'f', 6), QString::number(item.param2, 'f', 6),
        QString::number(item.param3, 'f', 6), QString::number(item.param4, 'f', 6),
        QString::number(item.x, 'f', 7), QString::number(item.y, 'f', 7),
        QString::number(item.z, 'f', 6), QString::number(item.autoContinue)}
        .join(QLatin1Char('\t'));
}

QString TlogExportService::MissionSignature(const TlogMissionItem &item)
{
    return QStringList{
        QString::number(item.sequence), QString::number(item.current),
        QString::number(item.frame), QString::number(item.command),
        QString::number(item.param1, 'g', 9), QString::number(item.param2, 'g', 9),
        QString::number(item.param3, 'g', 9), QString::number(item.param4, 'g', 9),
        QString::number(item.x, 'g', 17), QString::number(item.y, 'g', 17),
        QString::number(item.z, 'g', 17), QString::number(item.autoContinue)}
        .join(QLatin1Char(','));
}

QStringList TlogExportService::SnapshotOutputPaths(const QString &selectedOutput, int count)
{
    QStringList paths;
    const QFileInfo info(selectedOutput);
    const QString stem = info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString() : QStringLiteral(".") + info.suffix();
    for (int index = 0; index < count; ++index) {
        if (index == 0) {
            paths.append(selectedOutput);
        } else {
            paths.append(info.dir().filePath(
                QStringLiteral("%1-%2%3").arg(stem).arg(index + 1).arg(suffix)));
        }
    }
    return paths;
}

QString TlogExportService::KmlDocument(const QVector<TlogTrackPoint> &track)
{
    QString out;
    QXmlStreamWriter xml(&out);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("kml"));
    xml.writeDefaultNamespace(QStringLiteral("http://www.opengis.net/kml/2.2"));
    xml.writeStartElement(QStringLiteral("Document"));
    xml.writeStartElement(QStringLiteral("Folder"));
    xml.writeTextElement(QStringLiteral("name"), QStringLiteral("Flight Path"));
    xml.writeStartElement(QStringLiteral("Placemark"));
    xml.writeTextElement(QStringLiteral("name"), QStringLiteral("Flight Path"));
    xml.writeStartElement(QStringLiteral("LineString"));
    xml.writeTextElement(QStringLiteral("extrude"), QStringLiteral("1"));
    xml.writeTextElement(QStringLiteral("altitudeMode"), QStringLiteral("absolute"));
    QStringList coordinates;
    coordinates.reserve(track.size());
    for (const TlogTrackPoint &point : track) {
        coordinates.append(QStringLiteral("%1,%2,%3")
                               .arg(point.longitude, 0, 'f', 7)
                               .arg(point.latitude, 0, 'f', 7)
                               .arg(point.altitudeMeters, 0, 'f', 2));
    }
    xml.writeTextElement(QStringLiteral("coordinates"), coordinates.join(QLatin1Char('\n')));
    xml.writeEndElement(); // LineString
    xml.writeEndElement(); // Placemark
    xml.writeEndElement(); // Folder
    xml.writeEndElement(); // Document
    xml.writeEndElement(); // kml
    xml.writeEndDocument();
    return out;
}

QString TlogExportService::GpxDocument(const QVector<TlogTrackPoint> &track)
{
    QString out;
    QXmlStreamWriter xml(&out);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("gpx"));
    xml.writeDefaultNamespace(QStringLiteral("http://www.topografix.com/GPX/1/1"));
    xml.writeAttribute(QStringLiteral("version"), QStringLiteral("1.1"));
    xml.writeAttribute(QStringLiteral("creator"), QStringLiteral("APM Planner 3.0"));
    xml.writeStartElement(QStringLiteral("trk"));
    xml.writeTextElement(QStringLiteral("name"), QStringLiteral("Flight Path"));
    xml.writeStartElement(QStringLiteral("trkseg"));
    for (const TlogTrackPoint &point : track) {
        xml.writeStartElement(QStringLiteral("trkpt"));
        xml.writeAttribute(QStringLiteral("lat"), QString::number(point.latitude, 'f', 7));
        xml.writeAttribute(QStringLiteral("lon"), QString::number(point.longitude, 'f', 7));
        xml.writeTextElement(QStringLiteral("ele"), QString::number(point.altitudeMeters, 'f', 2));
        const qint64 msec = point.timestampUsec >= 0 ? point.timestampUsec / 1000
                                                     : -((-point.timestampUsec + 999) / 1000);
        xml.writeTextElement(QStringLiteral("time"),
                             QDateTime::fromMSecsSinceEpoch(msec, Qt::UTC)
                                     .toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"))
                                 + QLatin1Char('Z'));
        xml.writeEndElement(); // trkpt
    }
    xml.writeEndElement(); // trkseg
    xml.writeEndElement(); // trk
    xml.writeEndElement(); // gpx
    xml.writeEndDocument();
    return out;
}

QStringList TlogExportService::ParameterLines(const QMap<QString, TlogParameter> &parameters)
{
    QStringList lines; // QMap keyed by the upper-cased name == case-insensitive ordinal order
    for (auto it = parameters.constBegin(); it != parameters.constEnd(); ++it) {
        lines.append(it->name + QLatin1Char('\t') + FormatParameterValue(it->value));
    }
    return lines;
}

// --- extraction ----------------------------------------------------------------------------

QVector<TlogTrackPoint> TlogExportService::ReadTrack(TlogReader &reader, const CancelRequested &cancel)
{
    QVector<TlogTrackPoint> track;
    TlogRecord record;
    while (reader.next(&record) == TlogReader::Status::Ok) {
        if (cancel && cancel()) {
            break;
        }
        if (record.message.msgid != MAVLINK_MSG_ID_GLOBAL_POSITION_INT) {
            continue;
        }
        mavlink_global_position_int_t position;
        mavlink_msg_global_position_int_decode(&record.message, &position);
        const double latitude = position.lat / 1e7;
        const double longitude = position.lon / 1e7;
        if (!IsValidTrackPosition(latitude, longitude)) {
            continue;
        }
        TlogTrackPoint point;
        point.latitude = latitude;
        point.longitude = longitude;
        point.altitudeMeters = position.alt / 1000.0;
        point.timestampUsec = record.timestampUsec;
        track.append(point);
    }
    return track;
}

QMap<QString, TlogParameter> TlogExportService::ExtractParameters(TlogReader &reader,
                                                                 const CancelRequested &cancel)
{
    QSet<quint16> ardupilotSenders;
    QMap<QString, TlogParameter> values;
    TlogRecord record;
    while (reader.next(&record) == TlogReader::Status::Ok) {
        if (cancel && cancel()) {
            break;
        }
        const mavlink_message_t &message = record.message;
        if (message.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
            mavlink_heartbeat_t heartbeat;
            mavlink_msg_heartbeat_decode(&message, &heartbeat);
            if (heartbeat.autopilot == MAV_AUTOPILOT_ARDUPILOTMEGA) {
                ardupilotSenders.insert(endpointKey(message));
            }
            continue;
        }
        if (message.msgid != MAVLINK_MSG_ID_PARAM_VALUE) {
            continue;
        }
        mavlink_param_value_t parameter;
        mavlink_msg_param_value_decode(&message, &parameter);
        const QString name = parameterName(parameter);
        if (name.trimmed().isEmpty()) {
            continue;
        }
        // MP10: ArduPilot senders put the numeric value in the float; others use the
        // bytewise union encoding of the declared type.
        const ParameterEncoding encoding = ardupilotSenders.contains(endpointKey(message))
            ? ParameterEncoding::CStyleCast : ParameterEncoding::Bytewise;
        const ParameterType type = parameter.param_type >= 1 && parameter.param_type <= 10
            ? static_cast<ParameterType>(parameter.param_type) : ParameterType::Unknown;
        double value = parameter.param_value;
        if (type != ParameterType::Unknown) {
            bool ok = false;
            const QVariant decoded = ParameterCodec::decodeClassic(parameter.param_value, type,
                                                                   encoding, &ok);
            if (ok) {
                bool numeric = false;
                const double candidate = decoded.toDouble(&numeric);
                if (numeric) {
                    value = candidate;
                }
            }
        }
        const QString key = name.toUpper();
        auto it = values.find(key);
        if (it == values.end()) {
            values.insert(key, TlogParameter{name, value});
        } else {
            it->value = value; // first-seen spelling, latest value (MP10 OrdinalIgnoreCase dictionary)
        }
    }
    return values;
}

QVector<QVector<TlogMissionItem>> TlogExportService::ExtractMissionSnapshots(
    TlogReader &reader, const CancelRequested &cancel)
{
    QHash<quint16, MissionBuilder> builders;
    QVector<QVector<TlogMissionItem>> snapshots;
    QSet<QString> signatures;
    TlogRecord record;

    const auto store = [&](const mavlink_message_t &message, const TlogMissionItem &item) {
        const quint16 key = endpointKey(message);
        auto builder = builders.find(key);
        if (builder == builders.end() || item.sequence >= builder->expected) {
            return; // items without a MISSION_COUNT (or beyond it) never complete a mission
        }
        builder->items.insert(item.sequence, item);
        if (builder->items.size() != builder->expected) {
            return;
        }
        QVector<TlogMissionItem> mission;
        QStringList signature;
        for (auto it = builder->items.constBegin(); it != builder->items.constEnd(); ++it) {
            mission.append(*it);
            signature.append(MissionSignature(*it));
        }
        builders.erase(builder);
        const QString joined = signature.join(QLatin1Char('|'));
        if (signatures.contains(joined)) {
            return;
        }
        signatures.insert(joined);
        snapshots.append(mission);
    };

    while (reader.next(&record) == TlogReader::Status::Ok) {
        if (cancel && cancel()) {
            break;
        }
        const mavlink_message_t &message = record.message;
        const quint16 key = endpointKey(message);
        if (message.msgid == MAVLINK_MSG_ID_MISSION_COUNT) {
            mavlink_mission_count_t count;
            mavlink_msg_mission_count_decode(&message, &count);
            if (count.mission_type != MAV_MISSION_TYPE_MISSION || count.count == 0) {
                builders.remove(key);
                continue;
            }
            auto existing = builders.find(key);
            if (existing == builders.end() || existing->expected != count.count
                || existing->items.isEmpty()) {
                MissionBuilder builder;
                builder.expected = count.count;
                builders.insert(key, builder);
            }
            continue;
        }
        if (message.msgid == MAVLINK_MSG_ID_MISSION_ITEM_INT) {
            mavlink_mission_item_int_t raw;
            mavlink_msg_mission_item_int_decode(&message, &raw);
            if (raw.mission_type != MAV_MISSION_TYPE_MISSION) {
                continue;
            }
            TlogMissionItem item;
            item.sequence = raw.seq;
            item.current = raw.current;
            item.frame = raw.frame;
            item.command = raw.command;
            item.param1 = raw.param1;
            item.param2 = raw.param2;
            item.param3 = raw.param3;
            item.param4 = raw.param4;
            const double coordinateScale = IsGlobalFrame(raw.frame) ? 1e7 : 1e4;
            item.x = raw.x / coordinateScale;
            item.y = raw.y / coordinateScale;
            item.z = raw.z;
            item.autoContinue = raw.autocontinue;
            store(message, item);
            continue;
        }
        if (message.msgid == MAVLINK_MSG_ID_MISSION_ITEM) {
            mavlink_mission_item_t raw;
            mavlink_msg_mission_item_decode(&message, &raw);
            if (raw.mission_type != MAV_MISSION_TYPE_MISSION) {
                continue;
            }
            TlogMissionItem item;
            item.sequence = raw.seq;
            item.current = raw.current;
            item.frame = raw.frame;
            item.command = raw.command;
            item.param1 = raw.param1;
            item.param2 = raw.param2;
            item.param3 = raw.param3;
            item.param4 = raw.param4;
            item.x = raw.x;
            item.y = raw.y;
            item.z = raw.z;
            item.autoContinue = raw.autocontinue;
            store(message, item);
        }
    }
    return snapshots;
}

// --- export --------------------------------------------------------------------------------

TlogExportResult TlogExportService::Export(TlogExportFormat format, const QString &input,
                                           const QString &selectedOutput,
                                           const CancelRequested &cancel)
{
    return Export(format, input, selectedOutput, cancel, Progress());
}

TlogExportResult TlogExportService::Export(TlogExportFormat format, const QString &input,
                                           const QString &selectedOutput,
                                           const CancelRequested &cancel, const Progress &progress)
{
    if (selectedOutput.trimmed().isEmpty()) {
        return fail(QStringLiteral("No output file selected."));
    }
    if (comparablePath(input) == comparablePath(selectedOutput)) {
        return fail(QStringLiteral("The export destination must not replace the input tlog."));
    }
    QFile file(input);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("Cannot open %1: %2").arg(input, file.errorString()));
    }
    TlogReader reader(&file);
    if (reader.status() == TlogReader::Status::Error) {
        return fail(reader.errorString());
    }
    reader.setCancelCheck(cancel);
    reader.setProgress(progress);

    TlogExportResult result;
    const auto finishReader = [&result, &reader]() {
        result.recordsRead = reader.recordCount();
        result.skippedBytes = reader.skippedBytes();
    };
    const auto cancelled = [&result, &reader, format, &finishReader]() {
        finishReader();
        result.cancelled = true;
        result.message = CancelledText(format);
        return result;
    };
    const auto failed = [&result, &finishReader](const QString &error) {
        finishReader();
        result.error = error;
        return result;
    };
    const QString label = FormatLabel(format);

    switch (format) {
    case TlogExportFormat::Kml:
    case TlogExportFormat::Gpx: {
        const QVector<TlogTrackPoint> track = ReadTrack(reader, cancel);
        if (reader.status() == TlogReader::Status::Cancelled || (cancel && cancel())) {
            return cancelled();
        }
        if (reader.status() == TlogReader::Status::Error) {
            return failed(reader.errorString());
        }
        if (track.isEmpty()) {
            return failed(NoGpsPositionsText());
        }
        const QString document = format == TlogExportFormat::Kml ? KmlDocument(track)
                                                                  : GpxDocument(track);
        QString error;
        bool writeCancelled = false;
        if (!writeText(selectedOutput, document, cancel, &writeCancelled, &error)) {
            return writeCancelled ? cancelled() : failed(error);
        }
        finishReader();
        result.success = true;
        result.itemCount = track.size();
        result.outputPaths = QStringList{selectedOutput};
        result.message = QStringLiteral("Wrote %1: %2").arg(label, selectedOutput);
        return result;
    }
    case TlogExportFormat::Csv:
    case TlogExportFormat::Text: {
        const bool csv = format == TlogExportFormat::Csv;
        const QString delimiter = csv ? QStringLiteral(",") : QStringLiteral(" ");
        QSaveFile output(selectedOutput);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return failed(QStringLiteral("Cannot write %1: %2")
                              .arg(selectedOutput, output.errorString()));
        }
        int count = 0;
        TlogRecord record;
        while (reader.next(&record) == TlogReader::Status::Ok) {
            if (cancel && cancel()) {
                output.cancelWriting();
                return cancelled();
            }
            const QString text = DescribePacket(record.message, delimiter);
            if (text.isEmpty()) {
                continue;
            }
            const QString line = (csv ? FormatCsvTimestamp(record.timestampUsec)
                                      : FormatIsoTimestamp(record.timestampUsec))
                + delimiter + text + QLatin1Char('\n');
            const QByteArray bytes = line.toUtf8();
            if (output.write(bytes) != bytes.size()) {
                output.cancelWriting();
                return failed(QStringLiteral("Cannot write %1: %2")
                                  .arg(selectedOutput, output.errorString()));
            }
            ++count;
        }
        if (reader.status() == TlogReader::Status::Cancelled) {
            output.cancelWriting();
            return cancelled();
        }
        if (reader.status() == TlogReader::Status::Error) {
            output.cancelWriting();
            return failed(reader.errorString());
        }
        if (!output.commit()) {
            return failed(QStringLiteral("Cannot write %1: %2")
                              .arg(selectedOutput, output.errorString()));
        }
        finishReader();
        result.success = true;
        result.itemCount = count;
        result.outputPaths = QStringList{selectedOutput};
        result.message = QStringLiteral("Wrote %1 decoded packets to %2").arg(count).arg(selectedOutput);
        return result;
    }
    case TlogExportFormat::Parameters: {
        const QMap<QString, TlogParameter> parameters = ExtractParameters(reader, cancel);
        if (reader.status() == TlogReader::Status::Cancelled || (cancel && cancel())) {
            return cancelled();
        }
        if (reader.status() == TlogReader::Status::Error) {
            return failed(reader.errorString());
        }
        if (parameters.isEmpty()) {
            return failed(NoParametersText());
        }
        QString error;
        bool writeCancelled = false;
        if (!writeText(selectedOutput, ParameterLines(parameters).join(QLatin1Char('\n'))
                                           + QLatin1Char('\n'), cancel,
                       &writeCancelled, &error)) {
            return writeCancelled ? cancelled() : failed(error);
        }
        finishReader();
        result.success = true;
        result.itemCount = parameters.size();
        result.outputPaths = QStringList{selectedOutput};
        result.message = QStringLiteral("Wrote %1 parameters to %2").arg(parameters.size()).arg(selectedOutput);
        return result;
    }
    case TlogExportFormat::Missions: {
        const QVector<QVector<TlogMissionItem>> snapshots = ExtractMissionSnapshots(reader, cancel);
        if (reader.status() == TlogReader::Status::Cancelled || (cancel && cancel())) {
            return cancelled();
        }
        if (reader.status() == TlogReader::Status::Error) {
            return failed(reader.errorString());
        }
        if (snapshots.isEmpty()) {
            return failed(NoMissionText());
        }
        const QStringList paths = SnapshotOutputPaths(selectedOutput, snapshots.size());
        for (int index = 0; index < paths.size(); ++index) {
            if (comparablePath(paths.at(index)) == comparablePath(input)) {
                return failed(QStringLiteral(
                    "Mission snapshot output would replace the input tlog: %1")
                                  .arg(paths.at(index)));
            }
            // Only the selected first path went through Save As. Never silently
            // overwrite automatically generated siblings.
            if (index > 0 && QFileInfo::exists(paths.at(index))) {
                return failed(QStringLiteral(
                    "Mission snapshot output already exists: %1. Choose a different destination.")
                                  .arg(paths.at(index)));
            }
        }
        // Stage every snapshot before starting the per-file atomic commit phase.
        std::vector<std::unique_ptr<QSaveFile>> files;
        for (int index = 0; index < snapshots.size(); ++index) {
            if (cancel && cancel()) {
                for (auto &staged : files) {
                    staged->cancelWriting();
                }
                return cancelled();
            }
            auto saveFile = std::make_unique<QSaveFile>(paths.at(index));
            if (!saveFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                const QString error = QStringLiteral("Cannot write %1: %2")
                                          .arg(paths.at(index), saveFile->errorString());
                for (auto &staged : files) {
                    staged->cancelWriting();
                }
                return failed(error);
            }
            QString content = QStringLiteral("QGC WPL 110\n");
            for (const TlogMissionItem &item : snapshots.at(index)) {
                if (cancel && cancel()) {
                    saveFile->cancelWriting();
                    for (auto &staged : files) {
                        staged->cancelWriting();
                    }
                    return cancelled();
                }
                content += QgcWplLine(item) + QLatin1Char('\n');
            }
            const QByteArray bytes = content.toUtf8();
            qint64 offset = 0;
            while (offset < bytes.size()) {
                if (cancel && cancel()) {
                    saveFile->cancelWriting();
                    for (auto &staged : files) {
                        staged->cancelWriting();
                    }
                    return cancelled();
                }
                const qint64 chunk = qMin(
                    kWriteChunkBytes,
                    static_cast<qint64>(bytes.size()) - offset);
                const qint64 written = saveFile->write(
                    bytes.constData() + offset, chunk);
                if (written != chunk) {
                    const QString error = QStringLiteral("Cannot write %1: %2")
                                              .arg(paths.at(index), saveFile->errorString());
                    saveFile->cancelWriting();
                    for (auto &staged : files) {
                        staged->cancelWriting();
                    }
                    return failed(error);
                }
                offset += written;
            }
            files.push_back(std::move(saveFile));
        }
        if (cancel && cancel()) {
            for (auto &staged : files) {
                staged->cancelWriting();
            }
            return cancelled();
        }
        for (auto &staged : files) {
            if (!staged->commit()) {
                QString error = QStringLiteral("Cannot write %1: %2")
                                    .arg(staged->fileName(), staged->errorString());
                for (auto &other : files) {
                    other->cancelWriting(); // no-op for files already committed
                }
                if (!result.outputPaths.isEmpty()) {
                    error += QStringLiteral(" Earlier mission snapshots were committed: %1")
                                 .arg(result.outputPaths.join(QStringLiteral(", ")));
                }
                return failed(error);
            }
            result.outputPaths.append(staged->fileName());
        }
        finishReader();
        result.success = true;
        result.itemCount = snapshots.size();
        result.message = QStringLiteral("Wrote %1 unique mission snapshot(s): %2")
                             .arg(snapshots.size()).arg(result.outputPaths.join(QStringLiteral(", ")));
        return result;
    }
    }
    return fail(QStringLiteral("Unknown export format."));
}
