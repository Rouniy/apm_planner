// This define must precede the first MAVLink include in this translation unit
// so the generated dialect provides message and field wire metadata.
#ifndef MAVLINK_USE_MESSAGE_INFO
#define MAVLINK_USE_MESSAGE_INFO
#endif

#include "MavlinkGraphSampleExtractor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

const mavlink_field_info_t *findField(quint32 messageId,
                                      const QString &fieldName)
{
    const mavlink_message_info_t *info =
        mavlink_get_message_info_by_id(messageId);
    if (!info || fieldName.isEmpty()) {
        return nullptr;
    }

    const unsigned fieldCount =
        std::min(info->num_fields, static_cast<unsigned>(MAVLINK_MAX_FIELDS));
    for (unsigned index = 0; index < fieldCount; ++index) {
        const mavlink_field_info_t &field = info->fields[index];
        if (field.name && QString::fromLatin1(field.name) == fieldName) {
            return &field;
        }
    }
    return nullptr;
}

unsigned numericElementSize(mavlink_message_type_t type)
{
    switch (type) {
    case MAVLINK_TYPE_UINT8_T:
    case MAVLINK_TYPE_INT8_T:
        return 1;
    case MAVLINK_TYPE_UINT16_T:
    case MAVLINK_TYPE_INT16_T:
        return 2;
    case MAVLINK_TYPE_UINT32_T:
    case MAVLINK_TYPE_INT32_T:
    case MAVLINK_TYPE_FLOAT:
        return 4;
    case MAVLINK_TYPE_UINT64_T:
    case MAVLINK_TYPE_INT64_T:
    case MAVLINK_TYPE_DOUBLE:
        return 8;
    case MAVLINK_TYPE_CHAR:
        return 0;
    }
    return 0;
}

bool validNumericSpan(const mavlink_field_info_t &field,
                      unsigned *elementSize = nullptr,
                      unsigned *elementCount = nullptr)
{
    const unsigned size = numericElementSize(field.type);
    const unsigned count = field.array_length == 0
        ? 1U : field.array_length;
    if (size == 0 || count == 0
        || field.wire_offset >= MAVLINK_MAX_PAYLOAD_LEN
        || count > (MAVLINK_MAX_PAYLOAD_LEN - field.wire_offset) / size) {
        return false;
    }
    if (elementSize) {
        *elementSize = size;
    }
    if (elementCount) {
        *elementCount = count;
    }
    return true;
}

quint8 payloadByte(const mavlink_message_t &message, unsigned offset)
{
    if (offset >= MAVLINK_MAX_PAYLOAD_LEN || offset >= message.len) {
        // MAVLink 2 trims trailing zero bytes. Missing payload bytes decode as
        // zero and must never fall through into the adjacent checksum.
        return 0;
    }
    const auto *payload =
        reinterpret_cast<const quint8 *>(_MAV_PAYLOAD(&message));
    return payload[offset];
}

quint64 unsignedWireValue(const mavlink_message_t &message,
                          unsigned offset, unsigned size)
{
    quint64 value = 0;
    for (unsigned index = 0; index < size; ++index) {
        value |= static_cast<quint64>(payloadByte(message, offset + index))
                 << (8U * index);
    }
    return value;
}

template<typename Signed, typename Unsigned>
Signed signedFromBits(Unsigned bits)
{
    static_assert(sizeof(Signed) == sizeof(Unsigned),
                  "integer widths must match");
    Signed value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool readNumeric(const mavlink_message_t &message,
                 mavlink_message_type_t type, unsigned offset,
                 double *value)
{
    if (!value) {
        return false;
    }

    switch (type) {
    case MAVLINK_TYPE_UINT8_T:
        *value = static_cast<double>(payloadByte(message, offset));
        break;
    case MAVLINK_TYPE_INT8_T: {
        const quint8 bits = payloadByte(message, offset);
        *value = static_cast<double>(signedFromBits<qint8>(bits));
        break;
    }
    case MAVLINK_TYPE_UINT16_T:
        *value = static_cast<double>(static_cast<quint16>(
            unsignedWireValue(message, offset, 2)));
        break;
    case MAVLINK_TYPE_INT16_T: {
        const quint16 bits = static_cast<quint16>(
            unsignedWireValue(message, offset, 2));
        *value = static_cast<double>(signedFromBits<qint16>(bits));
        break;
    }
    case MAVLINK_TYPE_UINT32_T:
        *value = static_cast<double>(static_cast<quint32>(
            unsignedWireValue(message, offset, 4)));
        break;
    case MAVLINK_TYPE_INT32_T: {
        const quint32 bits = static_cast<quint32>(
            unsignedWireValue(message, offset, 4));
        *value = static_cast<double>(signedFromBits<qint32>(bits));
        break;
    }
    case MAVLINK_TYPE_UINT64_T:
        // MP10 uses IConvertible.ToDouble for UInt64. The conversion is
        // intentionally lossy above 2^53, unlike the text Inspector.
        *value = static_cast<double>(
            unsignedWireValue(message, offset, 8));
        break;
    case MAVLINK_TYPE_INT64_T: {
        const quint64 bits = unsignedWireValue(message, offset, 8);
        *value = static_cast<double>(signedFromBits<qint64>(bits));
        break;
    }
    case MAVLINK_TYPE_FLOAT: {
        const quint32 bits = static_cast<quint32>(
            unsignedWireValue(message, offset, 4));
        float decoded = 0.0F;
        std::memcpy(&decoded, &bits, sizeof(decoded));
        *value = static_cast<double>(decoded);
        break;
    }
    case MAVLINK_TYPE_DOUBLE: {
        const quint64 bits = unsignedWireValue(message, offset, 8);
        double decoded = 0.0;
        std::memcpy(&decoded, &bits, sizeof(decoded));
        *value = decoded;
        break;
    }
    case MAVLINK_TYPE_CHAR:
        return false;
    }
    return std::isfinite(*value);
}

} // namespace

bool MavlinkGraphSelection::matches(const mavlink_message_t &message) const
{
    return message.sysid == systemId
        && message.compid == componentId
        && message.msgid == messageId;
}

bool MavlinkGraphSampleExtractor::isSupportedField(
    quint32 messageId, const QString &fieldName)
{
    const mavlink_field_info_t *field = findField(messageId, fieldName);
    return field && validNumericSpan(*field);
}

bool MavlinkGraphSampleExtractor::tryRead(
    const mavlink_message_t &message, const QString &fieldName,
    QVector<double> *values)
{
    if (!values) {
        return false;
    }
    values->clear();
    const mavlink_field_info_t *field = findField(message.msgid, fieldName);
    unsigned elementSize = 0;
    unsigned elementCount = 0;
    if (!field || !validNumericSpan(
                      *field, &elementSize, &elementCount)) {
        return false;
    }

    QVector<double> decoded;
    decoded.reserve(static_cast<int>(elementCount));
    for (unsigned index = 0; index < elementCount; ++index) {
        double value = 0.0;
        if (!readNumeric(message, field->type,
                         field->wire_offset + index * elementSize,
                         &value)) {
            return false;
        }
        decoded.append(value);
    }
    *values = decoded;
    return !values->isEmpty();
}
