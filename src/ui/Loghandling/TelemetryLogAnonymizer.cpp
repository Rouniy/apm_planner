// Reflection is used only inside this worker translation unit.  It must be
// enabled before the first MAVLink header is included.
#ifndef MAVLINK_USE_MESSAGE_INFO
#define MAVLINK_USE_MESSAGE_INFO
#endif

#include "TelemetryLogAnonymizer.h"

#include "comm/TlogReader.h"

#include <QByteArrayList>
#include <QHash>
#include <QIODevice>
#include <QSet>
#include <QtEndian>

#include <mavlink_helpers.h>

#include <cmath>
#include <cstring>
#include <limits>

namespace
{

constexpr qint64 kMaximumTextLineBytes = 4LL * 1024LL * 1024LL;

enum class CoordinateKind { None, Latitude, Longitude };

struct TextSchema
{
    int latitude = -1;
    int longitude = -1;
};

bool validateDevices(QIODevice *input, QIODevice *output,
                     const LogAnonymizeOptions &options,
                     LogAnonymizeResult *result)
{
    if (!result) return false;
    if (!input || !output) {
        result->error = QStringLiteral("Both input and staging output devices are required.");
        return false;
    }
    if (input == output) {
        result->error = QStringLiteral("Input and staging output must be different devices.");
        return false;
    }
    if (!input->isOpen() || !input->isReadable() || input->isSequential()) {
        result->error = QStringLiteral("The input log must be open, readable and seekable.");
        return false;
    }
    if (!output->isOpen() || !output->isWritable()) {
        result->error = QStringLiteral("The staging output is not open for writing.");
        return false;
    }
    if (!std::isfinite(options.latitudeOffset)
        || !std::isfinite(options.longitudeOffset)) {
        result->error = QStringLiteral("Coordinate offsets must be finite numbers.");
        return false;
    }
    if (!input->seek(0)) {
        result->error = QStringLiteral("Could not rewind the input log: %1")
                            .arg(input->errorString());
        return false;
    }
    result->inputBytes = input->size();
    return true;
}

bool writeAll(QIODevice *output, const QByteArray &bytes,
              LogAnonymizeResult *result)
{
    qint64 offset = 0;
    while (offset < bytes.size()) {
        const qint64 written = output->write(
            bytes.constData() + offset, bytes.size() - offset);
        if (written <= 0) {
            result->error = QStringLiteral("Could not write the staging log: %1")
                                .arg(output->errorString());
            return false;
        }
        offset += written;
        result->outputBytes += written;
    }
    return true;
}

bool readExactAt(QIODevice *input, qint64 offset, qint64 count,
                 QByteArray *bytes, QString *error)
{
    if (!bytes || count < 0 || count > std::numeric_limits<int>::max()
        || !input->seek(offset)) {
        if (error) {
            *error = QStringLiteral("Could not seek in the telemetry log: %1")
                         .arg(input ? input->errorString() : QString());
        }
        return false;
    }
    bytes->clear();
    bytes->reserve(static_cast<int>(count));
    while (bytes->size() < count) {
        const QByteArray chunk = input->read(count - bytes->size());
        if (chunk.isEmpty()) {
            if (error) {
                *error = QStringLiteral("The telemetry log changed or could not be read: %1")
                             .arg(input->errorString());
            }
            return false;
        }
        bytes->append(chunk);
    }
    return true;
}

CoordinateKind referenceCoordinateKind(const char *name)
{
    if (!name) return CoordinateKind::None;
    const QByteArray field(name);
    static const QSet<QByteArray> latitudeNames = {
        QByteArrayLiteral("lat"), QByteArrayLiteral("latitude"),
        QByteArrayLiteral("lat_int"), QByteArrayLiteral("landing_lat"),
        QByteArrayLiteral("path_lat"), QByteArrayLiteral("arc_entry_lat"),
        QByteArrayLiteral("gpsLat"), QByteArrayLiteral("gpsOffsetLat")};
    static const QSet<QByteArray> longitudeNames = {
        QByteArrayLiteral("lon"), QByteArrayLiteral("lng"),
        QByteArrayLiteral("longitude"),
        QByteArrayLiteral("lon_int"), QByteArrayLiteral("landing_lon"),
        QByteArrayLiteral("path_lon"), QByteArrayLiteral("arc_entry_lon"),
        QByteArrayLiteral("gpsLon"), QByteArrayLiteral("gpsOffsetLon")};
    if (latitudeNames.contains(field)) return CoordinateKind::Latitude;
    if (longitudeNames.contains(field)) return CoordinateKind::Longitude;
    return CoordinateKind::None;
}

bool isGlobalFrame(quint8 frame)
{
    switch (frame) {
    case MAV_FRAME_GLOBAL:
    case MAV_FRAME_GLOBAL_RELATIVE_ALT:
    case MAV_FRAME_GLOBAL_INT:
    case MAV_FRAME_GLOBAL_RELATIVE_ALT_INT:
    case MAV_FRAME_GLOBAL_TERRAIN_ALT:
    case MAV_FRAME_GLOBAL_TERRAIN_ALT_INT:
        return true;
    default:
        return false;
    }
}

bool isLocationCommand(quint16 command, float param1)
{
    // MISSION_ITEM x/y are overloaded command parameters.  Keep this an
    // explicit allowlist so local offsets, identifiers and non-location NAV
    // parameters can never be translated accidentally.  COMMAND_LONG has no
    // coordinate-frame field and therefore remains under the UI's documented
    // "unrecognized location fields may remain" warning.
    switch (command) {
    case MAV_CMD_NAV_WAYPOINT:
    case MAV_CMD_NAV_LOITER_UNLIM:
    case MAV_CMD_NAV_LOITER_TURNS:
    case MAV_CMD_NAV_LOITER_TIME:
    case MAV_CMD_NAV_LAND:
    case MAV_CMD_NAV_TAKEOFF:
    case MAV_CMD_NAV_FOLLOW:
    case MAV_CMD_NAV_LOITER_TO_ALT:
    case MAV_CMD_NAV_PATHPLANNING:
    case MAV_CMD_NAV_SPLINE_WAYPOINT:
    case MAV_CMD_NAV_VTOL_TAKEOFF:
    case MAV_CMD_NAV_VTOL_LAND:
    case MAV_CMD_NAV_PAYLOAD_PLACE:
    case MAV_CMD_DO_LAND_START:
    case MAV_CMD_DO_REPOSITION:
    case MAV_CMD_DO_SET_ROI_LOCATION:
    case MAV_CMD_NAV_FENCE_RETURN_POINT:
    case MAV_CMD_NAV_FENCE_POLYGON_VERTEX_INCLUSION:
    case MAV_CMD_NAV_FENCE_POLYGON_VERTEX_EXCLUSION:
    case MAV_CMD_NAV_FENCE_CIRCLE_INCLUSION:
    case MAV_CMD_NAV_FENCE_CIRCLE_EXCLUSION:
    case MAV_CMD_NAV_RALLY_POINT:
        return true;
    case MAV_CMD_DO_SET_HOME:
        // Param 1 selects the current location when non-zero.
        return param1 == 0.0F;
    case MAV_CMD_NAV_ROI:
    case MAV_CMD_DO_SET_ROI:
        return param1 == static_cast<float>(MAV_ROI_LOCATION);
    default:
        return false;
    }
}

bool missionItemCarriesGlobalLocation(const mavlink_message_t &message)
{
    quint8 frame = 0;
    quint16 command = 0;
    float param1 = 0.0F;
    if (message.msgid == MAVLINK_MSG_ID_MISSION_ITEM) {
        frame = mavlink_msg_mission_item_get_frame(&message);
        command = mavlink_msg_mission_item_get_command(&message);
        param1 = mavlink_msg_mission_item_get_param1(&message);
    } else if (message.msgid == MAVLINK_MSG_ID_MISSION_ITEM_INT) {
        frame = mavlink_msg_mission_item_int_get_frame(&message);
        command = mavlink_msg_mission_item_int_get_command(&message);
        param1 = mavlink_msg_mission_item_int_get_param1(&message);
    } else {
        return false;
    }
    return isGlobalFrame(frame) && isLocationCommand(command, param1);
}

CoordinateKind messageCoordinateKind(const mavlink_message_t &message,
                                     const mavlink_field_info_t &field,
                                     bool missionLocation)
{
    const CoordinateKind referenceKind = referenceCoordinateKind(field.name);
    if (referenceKind != CoordinateKind::None) return referenceKind;
    if (!missionLocation || !field.name) return CoordinateKind::None;
    if (std::strcmp(field.name, "x") == 0) return CoordinateKind::Latitude;
    if (std::strcmp(field.name, "y") == 0) return CoordinateKind::Longitude;
    return CoordinateKind::None;
}

bool isKnownNonCoordinateReferenceField(const mavlink_message_t &message,
                                        const mavlink_field_info_t &field)
{
    // Privacy.cs names these fields, but in UAVIONIX_ADSB_OUT_CFG they are
    // uint8 antenna-offset enums, not latitude/longitude values.
    return message.msgid == MAVLINK_MSG_ID_UAVIONIX_ADSB_OUT_CFG
        && field.type == MAVLINK_TYPE_UINT8_T
        && field.name
        && (std::strcmp(field.name, "gpsOffsetLat") == 0
            || std::strcmp(field.name, "gpsOffsetLon") == 0);
}

bool isReferenceBlockedMessage(quint32 messageId)
{
    switch (messageId) {
    case MAVLINK_MSG_ID_FENCE_POINT:
    case MAVLINK_MSG_ID_SIMSTATE:
    case MAVLINK_MSG_ID_RALLY_POINT:
    case MAVLINK_MSG_ID_AHRS2:
    case MAVLINK_MSG_ID_CAMERA_FEEDBACK:
    case MAVLINK_MSG_ID_AHRS3:
    case MAVLINK_MSG_ID_DEEPSTALL:
    case MAVLINK_MSG_ID_GPS_RAW_INT:
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT:
    case MAVLINK_MSG_ID_SET_GPS_GLOBAL_ORIGIN:
    case MAVLINK_MSG_ID_GPS_GLOBAL_ORIGIN:
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT_COV:
    case MAVLINK_MSG_ID_SET_POSITION_TARGET_GLOBAL_INT:
    case MAVLINK_MSG_ID_HIL_STATE:
    case MAVLINK_MSG_ID_SIM_STATE:
    case MAVLINK_MSG_ID_HIL_GPS:
    case MAVLINK_MSG_ID_HIL_STATE_QUATERNION:
    case MAVLINK_MSG_ID_GPS2_RAW:
    case MAVLINK_MSG_ID_TERRAIN_REQUEST:
    case MAVLINK_MSG_ID_TERRAIN_CHECK:
    case MAVLINK_MSG_ID_TERRAIN_REPORT:
    case MAVLINK_MSG_ID_FOLLOW_TARGET:
    case MAVLINK_MSG_ID_GPS_INPUT:
    case MAVLINK_MSG_ID_HIGH_LATENCY:
    case MAVLINK_MSG_ID_HOME_POSITION:
    case MAVLINK_MSG_ID_SET_HOME_POSITION:
    case MAVLINK_MSG_ID_ADSB_VEHICLE:
    case MAVLINK_MSG_ID_CAMERA_IMAGE_CAPTURED:
    case MAVLINK_MSG_ID_UAVIONIX_ADSB_OUT_DYNAMIC:
    case MAVLINK_MSG_ID_LOCAL_POSITION_NED:
    case MAVLINK_MSG_ID_COMMAND_LONG:
    case MAVLINK_MSG_ID_MISSION_ITEM:
    case MAVLINK_MSG_ID_MISSION_ITEM_INT:
    case MAVLINK_MSG_ID_UAVIONIX_ADSB_OUT_CFG:
        return true;
    default:
        return false;
    }
}

bool patchInt32(char *payload, unsigned int offset, double degreeOffset,
                bool *changed, QString *error)
{
    const qint32 original = qFromLittleEndian<qint32>(
        reinterpret_cast<const uchar *>(payload + offset));
    if (original == 0 || degreeOffset == 0.0) return true;
    const long double shifted = static_cast<long double>(original)
        + static_cast<long double>(degreeOffset) * 10000000.0L;
    if (!std::isfinite(shifted)
        || shifted < std::numeric_limits<qint32>::min()
        || shifted > std::numeric_limits<qint32>::max()) {
        if (error) *error = QStringLiteral("A shifted MAVLink coordinate overflows int32.");
        return false;
    }
    const qint32 replacement = static_cast<qint32>(shifted);
    qToLittleEndian<qint32>(
        replacement, reinterpret_cast<uchar *>(payload + offset));
    if (changed) *changed = replacement != original;
    return true;
}

bool patchFloat(char *payload, unsigned int offset, double degreeOffset,
                bool *changed, QString *error)
{
    const quint32 bits = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar *>(payload + offset));
    float original = 0.0F;
    std::memcpy(&original, &bits, sizeof(original));
    if (!std::isfinite(original)) {
        if (error) *error = QStringLiteral("A MAVLink coordinate is not finite.");
        return false;
    }
    if (original == 0.0F || degreeOffset == 0.0) return true;
    const double shifted = static_cast<double>(original) + degreeOffset;
    if (!std::isfinite(shifted)
        || std::abs(shifted) > std::numeric_limits<float>::max()) {
        if (error) *error = QStringLiteral("A shifted MAVLink coordinate overflows float.");
        return false;
    }
    const float replacement = static_cast<float>(shifted);
    if (!std::isfinite(replacement)) {
        if (error) *error = QStringLiteral("A shifted MAVLink coordinate is not finite.");
        return false;
    }
    quint32 replacementBits = 0;
    std::memcpy(&replacementBits, &replacement, sizeof(replacement));
    qToLittleEndian<quint32>(
        replacementBits, reinterpret_cast<uchar *>(payload + offset));
    if (changed) *changed = replacementBits != bits;
    return true;
}

bool patchMessage(mavlink_message_t *message,
                  const mavlink_message_info_t *info,
                  const LogAnonymizeOptions &options,
                  LogAnonymizeResult *result,
                  bool *messageChanged)
{
    if (!message || !info || !result || !messageChanged) return false;
    *messageChanged = false;
    const quint8 minimumLength = mavlink_min_message_length(message);
    const quint8 maximumLength = mavlink_max_message_length(message);
    if (maximumLength == 0 || minimumLength > maximumLength
        || message->len > maximumLength
        || (message->magic == MAVLINK_STX_MAVLINK1
            && message->len != minimumLength)) {
        result->error = QStringLiteral(
            "A MAVLink message has no valid payload length in the configured dialect.");
        return false;
    }
    char *const payload = _MAV_PAYLOAD_NON_CONST(message);
    // MAVLink 2 can trim any number of zero high bytes from the final scalar,
    // not only entire trailing fields.  Materialize the logical payload before
    // reading coordinates so a partially transmitted non-zero scalar is kept.
    if (message->len < maximumLength) {
        std::memset(payload + message->len, 0, maximumLength - message->len);
    }
    if (!isReferenceBlockedMessage(message->msgid)) {
        for (unsigned int index = 0; index < info->num_fields; ++index) {
            if (referenceCoordinateKind(info->fields[index].name) == CoordinateKind::None)
                continue;
            const QString warning = QStringLiteral(
                "Coordinates in %1 (MAVLink id %2) were left unchanged: this message is outside the current coordinate transform coverage.")
                .arg(QString::fromLatin1(info->name)).arg(message->msgid);
            if (!result->warnings.contains(warning)) result->warnings.append(warning);
            break;
        }
        return true;
    }
    const bool missionLocation = missionItemCarriesGlobalLocation(*message);
    for (unsigned int index = 0; index < info->num_fields; ++index) {
        const mavlink_field_info_t &field = info->fields[index];
        const CoordinateKind kind = messageCoordinateKind(
            *message, field, missionLocation);
        if (kind == CoordinateKind::None) continue;
        if (isKnownNonCoordinateReferenceField(*message, field)) continue;
        ++result->coordinateFields;
        if (field.array_length != 0
            || (field.type != MAVLINK_TYPE_INT32_T
                && field.type != MAVLINK_TYPE_FLOAT)) {
            result->error = QStringLiteral(
                "Coordinate field %1 in MAVLink message %2 has an unsupported encoding.")
                                .arg(QString::fromLatin1(field.name))
                                .arg(message->msgid);
            return false;
        }
        constexpr unsigned int width = 4U;
        if (field.wire_offset + width > maximumLength
            || (message->magic == MAVLINK_STX_MAVLINK1
                && field.wire_offset < message->len
                && field.wire_offset + width > message->len)) {
            result->error = QStringLiteral(
                "Coordinate field %1 exceeds the MAVLink payload boundary.")
                                .arg(QString::fromLatin1(field.name));
            return false;
        }
        bool fieldChanged = false;
        const double offset = kind == CoordinateKind::Latitude
            ? options.latitudeOffset : options.longitudeOffset;
        bool patched = true;
        if (field.type == MAVLINK_TYPE_INT32_T) {
            patched = patchInt32(payload, field.wire_offset, offset,
                                 &fieldChanged, &result->error);
        } else {
            patched = patchFloat(payload, field.wire_offset, offset,
                                 &fieldChanged, &result->error);
        }
        if (!patched) return false;
        if (fieldChanged) {
            *messageChanged = true;
            ++result->patchedValues;
        }
    }
    if (*messageChanged && message->magic != MAVLINK_STX_MAVLINK1) {
        message->len = _mav_trim_payload(payload, maximumLength);
    }
    return true;
}

void recalculateChecksum(mavlink_message_t *message)
{
    uchar header[9]{};
    int headerLength = 0;
    header[headerLength++] = message->len;
    if (message->magic == MAVLINK_STX_MAVLINK1) {
        header[headerLength++] = message->seq;
        header[headerLength++] = message->sysid;
        header[headerLength++] = message->compid;
        header[headerLength++] = static_cast<uchar>(message->msgid);
    } else {
        header[headerLength++] = message->incompat_flags;
        header[headerLength++] = message->compat_flags;
        header[headerLength++] = message->seq;
        header[headerLength++] = message->sysid;
        header[headerLength++] = message->compid;
        header[headerLength++] = static_cast<uchar>(message->msgid);
        header[headerLength++] = static_cast<uchar>(message->msgid >> 8);
        header[headerLength++] = static_cast<uchar>(message->msgid >> 16);
    }
    quint16 checksum = crc_calculate(header, headerLength);
    crc_accumulate_buffer(&checksum, _MAV_PAYLOAD(message), message->len);
    crc_accumulate(mavlink_get_crc_extra(message), &checksum);
    message->checksum = checksum;
}

QByteArray serializeMessage(const mavlink_message_t &message)
{
    const bool mavlink1 = message.magic == MAVLINK_STX_MAVLINK1;
    const bool signedFrame = !mavlink1
        && (message.incompat_flags & MAVLINK_IFLAG_SIGNED) != 0;
    const int headerBytes = mavlink1 ? 6 : 10;
    QByteArray bytes(headerBytes + message.len + 2
                         + (signedFrame ? MAVLINK_SIGNATURE_BLOCK_LEN : 0),
                     char(0));
    uchar *const out = reinterpret_cast<uchar *>(bytes.data());
    out[0] = message.magic;
    out[1] = message.len;
    if (mavlink1) {
        out[2] = message.seq;
        out[3] = message.sysid;
        out[4] = message.compid;
        out[5] = static_cast<uchar>(message.msgid);
    } else {
        out[2] = message.incompat_flags;
        out[3] = message.compat_flags;
        out[4] = message.seq;
        out[5] = message.sysid;
        out[6] = message.compid;
        out[7] = static_cast<uchar>(message.msgid);
        out[8] = static_cast<uchar>(message.msgid >> 8);
        out[9] = static_cast<uchar>(message.msgid >> 16);
    }
    std::memcpy(out + headerBytes, _MAV_PAYLOAD(&message), message.len);
    out[headerBytes + message.len] = static_cast<uchar>(message.checksum);
    out[headerBytes + message.len + 1] =
        static_cast<uchar>(message.checksum >> 8);
    if (signedFrame) {
        std::memcpy(out + headerBytes + message.len + 2,
                    message.signature, MAVLINK_SIGNATURE_BLOCK_LEN);
    }
    return bytes;
}

QByteArrayList commaFields(const QByteArray &line)
{
    QByteArray body = line;
    if (body.endsWith('\n')) body.chop(1);
    if (body.endsWith('\r')) body.chop(1);
    return body.split(',');
}

QByteArray trimmedToken(const QByteArray &token)
{
    return token.trimmed();
}

bool replaceTextCoordinate(QByteArrayList *fields, int index,
                           double offset, LogAnonymizeResult *result)
{
    if (!fields || index < 0) return true;
    if (index >= fields->size()) {
        result->error = QStringLiteral("A DataFlash record is missing a declared coordinate column.");
        return false;
    }
    QByteArray &field = (*fields)[index];
    const QByteArray valueBytes = field.trimmed();
    if (valueBytes.isEmpty()) {
        result->error = QStringLiteral("A DataFlash coordinate value is empty.");
        return false;
    }
    bool parsed = false;
    const double original = valueBytes.toDouble(&parsed);
    if (!parsed || !std::isfinite(original)) {
        result->error = QStringLiteral("A DataFlash coordinate is malformed or not finite: %1")
                            .arg(QString::fromLatin1(valueBytes));
        return false;
    }
    ++result->coordinateFields;
    if (original == 0.0 || offset == 0.0) return true;
    const double shifted = original + offset;
    if (!std::isfinite(shifted)) {
        result->error = QStringLiteral("A shifted DataFlash coordinate is not finite.");
        return false;
    }
    int leading = 0;
    while (leading < field.size()
           && (field.at(leading) == ' ' || field.at(leading) == '\t')) {
        ++leading;
    }
    int trailing = field.size();
    while (trailing > leading
           && (field.at(trailing - 1) == ' '
               || field.at(trailing - 1) == '\t')) {
        --trailing;
    }
    field = field.left(leading) + QByteArray::number(shifted, 'g', 17)
        + field.mid(trailing);
    ++result->patchedValues;
    return true;
}

QByteArray newlineSuffix(const QByteArray &line)
{
    if (line.endsWith("\r\n")) return QByteArrayLiteral("\r\n");
    if (line.endsWith('\n')) return QByteArrayLiteral("\n");
    return {};
}

} // namespace

LogAnonymizeResult TelemetryLogAnonymizer::anonymizeText(
    QIODevice *input, QIODevice *output,
    const LogAnonymizeOptions &options,
    const LogAnonymizeCancel &cancel,
    const LogAnonymizeProgress &progress)
{
    LogAnonymizeResult result;
    if (!validateDevices(input, output, options, &result)) return result;
    const qint64 total = input->size();
    QHash<QByteArray, TextSchema> schemas;
    if (progress) progress(0, total);

    while (!input->atEnd()) {
        if (cancel && cancel()) {
            result.cancelled = true;
            return result;
        }
        QByteArray line = input->readLine(kMaximumTextLineBytes + 1);
        if (line.isEmpty() && !input->atEnd()) {
            result.error = QStringLiteral("Could not read the DataFlash log: %1")
                               .arg(input->errorString());
            return result;
        }
        if (line.size() > kMaximumTextLineBytes
            || (!line.endsWith('\n') && !input->atEnd())) {
            result.error = QStringLiteral("A DataFlash text line exceeds the 4 MiB safety limit.");
            return result;
        }
        const QByteArray newline = newlineSuffix(line);
        QByteArrayList fields = commaFields(line);
        const QByteArray recordName = fields.isEmpty()
            ? QByteArray() : trimmedToken(fields.first());
        if (recordName == QByteArrayLiteral("FMT")) {
            if (fields.size() < 5) {
                result.error = QStringLiteral("A DataFlash FMT record is malformed.");
                return result;
            }
            const QByteArray describedName = trimmedToken(fields.at(3));
            if (describedName.isEmpty()) {
                result.error = QStringLiteral("A DataFlash FMT record has no message name.");
                return result;
            }
            TextSchema schema;
            for (int index = 5; index < fields.size(); ++index) {
                const QByteArray column = trimmedToken(fields.at(index));
                if (column == QByteArrayLiteral("Lat")) {
                    if (schema.latitude >= 0) {
                        result.error = QStringLiteral("A DataFlash FMT record declares Lat twice.");
                        return result;
                    }
                    schema.latitude = index - 4;
                } else if (column == QByteArrayLiteral("Lng")) {
                    if (schema.longitude >= 0) {
                        result.error = QStringLiteral("A DataFlash FMT record declares Lng twice.");
                        return result;
                    }
                    schema.longitude = index - 4;
                }
            }
            schemas.insert(describedName, schema);
        } else {
            const auto schema = schemas.constFind(recordName);
            if (schema != schemas.constEnd()
                && (schema->latitude >= 0 || schema->longitude >= 0)) {
                if (!replaceTextCoordinate(
                        &fields, schema->latitude, options.latitudeOffset, &result)
                    || !replaceTextCoordinate(
                        &fields, schema->longitude, options.longitudeOffset, &result)) {
                    return result;
                }
                line = fields.join(QByteArrayLiteral(",")) + newline;
            }
        }
        if (!writeAll(output, line, &result)) return result;
        ++result.records;
        if (progress) progress(input->pos(), total);
    }
    if (cancel && cancel()) {
        result.cancelled = true;
        return result;
    }
    if (schemas.isEmpty()) {
        result.error = QStringLiteral("No DataFlash FMT definitions were found; format is chosen from the input extension.");
        return result;
    }
    result.success = true;
    if (progress) progress(total, total);
    return result;
}

LogAnonymizeResult TelemetryLogAnonymizer::anonymizeTlog(
    QIODevice *input, QIODevice *output,
    const LogAnonymizeOptions &options,
    const LogAnonymizeCancel &cancel,
    const LogAnonymizeProgress &progress)
{
    LogAnonymizeResult result;
    if (!validateDevices(input, output, options, &result)) return result;
    const qint64 total = input->size();
    if (progress) progress(0, total);
    TlogReader reader(input);
    reader.setCancelCheck(cancel);
    qint64 expectedOffset = 0;
    bool signatureWarningAdded = false;

    for (;;) {
        TlogRecord record;
        const TlogReader::Status status = reader.next(&record);
        if (status == TlogReader::Status::End) break;
        if (status == TlogReader::Status::Cancelled) {
            result.cancelled = true;
            return result;
        }
        if (status != TlogReader::Status::Ok) {
            result.error = reader.errorString().isEmpty()
                ? QStringLiteral("The telemetry log contains corrupt or truncated framing.")
                : reader.errorString();
            return result;
        }
        if (record.offset != expectedOffset || reader.skippedBytes() != 0
            || reader.rejectedFrames() != 0) {
            result.error = QStringLiteral(
                "The telemetry log contains unrecognized or corrupt framing at byte %1.")
                               .arg(expectedOffset);
            return result;
        }
        const qint64 recordEnd = reader.bytesProcessed();
        const qint64 recordBytes = recordEnd - record.offset;
        QByteArray original;
        if (recordBytes < TlogReader::TimestampBytes
            + TlogReader::MinimumFrameBytes
            || !readExactAt(input, record.offset, recordBytes,
                            &original, &result.error)) {
            if (result.error.isEmpty()) {
                result.error = QStringLiteral("A telemetry record has an invalid length.");
            }
            return result;
        }
        expectedOffset = recordEnd;

        const mavlink_message_info_t *const info =
            mavlink_get_message_info_by_id(record.message.msgid);
        if (!info) {
            result.error = QStringLiteral(
                "MAVLink message id %1 is unavailable in the configured dialect; the log was not published.")
                               .arg(record.message.msgid);
            return result;
        }
        const bool mavlink1 =
            record.message.magic == MAVLINK_STX_MAVLINK1;
        if (!mavlink1 && record.message.magic != MAVLINK_STX) {
            result.error = QStringLiteral("A telemetry record has an unsupported MAVLink version.");
            return result;
        }
        const int expectedFrameBytes = (mavlink1 ? 6 : 10)
            + record.message.len + 2
            + ((!mavlink1
                && (record.message.incompat_flags & MAVLINK_IFLAG_SIGNED))
                   ? MAVLINK_SIGNATURE_BLOCK_LEN : 0);
        if (recordBytes != TlogReader::TimestampBytes + expectedFrameBytes) {
            result.error = QStringLiteral("A telemetry record has inconsistent framing length.");
            return result;
        }

        mavlink_message_t patched = record.message;
        bool changed = false;
        if (!patchMessage(&patched, info, options, &result, &changed)) {
            return result;
        }
        QByteArray outputRecord;
        if (!changed) {
            outputRecord = original;
        } else {
            if (!mavlink1
                && (patched.incompat_flags & MAVLINK_IFLAG_SIGNED)) {
                patched.incompat_flags &= ~MAVLINK_IFLAG_SIGNED;
                std::memset(patched.signature, 0,
                            MAVLINK_SIGNATURE_BLOCK_LEN);
                ++result.strippedSignatures;
                if (!signatureWarningAdded) {
                    result.warnings.append(QStringLiteral(
                        "Signatures were stripped only from modified MAVLink 2 frames; signatures were not verified or regenerated."));
                    signatureWarningAdded = true;
                }
            }
            recalculateChecksum(&patched);
            outputRecord = original.left(TlogReader::TimestampBytes)
                + serializeMessage(patched);
        }
        if (!writeAll(output, outputRecord, &result)) return result;
        ++result.records;
        if (progress) progress(recordEnd, total);
    }
    if (expectedOffset != total || reader.skippedBytes() != 0
        || reader.rejectedFrames() != 0) {
        result.error = QStringLiteral(
            "The telemetry log contains trailing, unrecognized or corrupt data.");
        return result;
    }
    if (cancel && cancel()) {
        result.cancelled = true;
        return result;
    }
    result.success = true;
    if (progress) progress(total, total);
    return result;
}
