// MAVLINK_USE_MESSAGE_INFO must precede the first MAVLink include so that the
// generated dialect exposes message names and field wire layouts in this TU.
#ifndef MAVLINK_USE_MESSAGE_INFO
#define MAVLINK_USE_MESSAGE_INFO
#endif

#include "MAVLinkInspectorPacketStore.h"

#include <QByteArray>
#include <QStringList>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>

namespace {

unsigned wireElementSize(mavlink_message_type_t type)
{
    switch (type) {
    case MAVLINK_TYPE_CHAR:
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
    }
    return 0;
}

QString scalarTypeName(mavlink_message_type_t type)
{
    switch (type) {
    case MAVLINK_TYPE_CHAR:
        return QStringLiteral("char");
    case MAVLINK_TYPE_UINT8_T:
        return QStringLiteral("uint8_t");
    case MAVLINK_TYPE_INT8_T:
        return QStringLiteral("int8_t");
    case MAVLINK_TYPE_UINT16_T:
        return QStringLiteral("uint16_t");
    case MAVLINK_TYPE_INT16_T:
        return QStringLiteral("int16_t");
    case MAVLINK_TYPE_UINT32_T:
        return QStringLiteral("uint32_t");
    case MAVLINK_TYPE_INT32_T:
        return QStringLiteral("int32_t");
    case MAVLINK_TYPE_UINT64_T:
        return QStringLiteral("uint64_t");
    case MAVLINK_TYPE_INT64_T:
        return QStringLiteral("int64_t");
    case MAVLINK_TYPE_FLOAT:
        return QStringLiteral("float");
    case MAVLINK_TYPE_DOUBLE:
        return QStringLiteral("double");
    }
    return QStringLiteral("unknown");
}

QString fieldTypeName(const mavlink_field_info_t &field)
{
    const QString scalar = scalarTypeName(field.type);
    if (field.array_length == 0) {
        return scalar;
    }
    return QStringLiteral("%1[%2]").arg(scalar).arg(field.array_length);
}

quint8 payloadByte(const mavlink_message_t &message, unsigned offset)
{
    if (offset >= MAVLINK_MAX_PAYLOAD_LEN || offset >= message.len) {
        // MAVLink 2 removes trailing zeroes. Treat every omitted payload byte
        // as zero, without falling through into the adjacent checksum bytes.
        return 0;
    }
    const auto *payload =
        reinterpret_cast<const quint8 *>(_MAV_PAYLOAD(&message));
    return payload[offset];
}

quint64 unsignedWireValue(const mavlink_message_t &message, unsigned offset,
                          unsigned size)
{
    quint64 value = 0;
    for (unsigned index = 0; index < size; ++index) {
        value |= static_cast<quint64>(payloadByte(message, offset + index))
                 << (index * 8U);
    }
    return value;
}

template<typename Signed, typename Unsigned>
Signed signedFromBits(Unsigned bits)
{
    static_assert(sizeof(Signed) == sizeof(Unsigned),
                  "signed and unsigned values must have the same width");
    Signed value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

QString scalarValue(const mavlink_message_t &message,
                    mavlink_message_type_t type, unsigned offset)
{
    switch (type) {
    case MAVLINK_TYPE_CHAR: {
        const char value = static_cast<char>(payloadByte(message, offset));
        return value == '\0' ? QString()
                             : QString::fromLatin1(&value, 1);
    }
    case MAVLINK_TYPE_UINT8_T:
        return QString::number(payloadByte(message, offset));
    case MAVLINK_TYPE_INT8_T: {
        const quint8 bits = payloadByte(message, offset);
        return QString::number(signedFromBits<qint8>(bits));
    }
    case MAVLINK_TYPE_UINT16_T:
        return QString::number(static_cast<quint16>(
            unsignedWireValue(message, offset, 2)));
    case MAVLINK_TYPE_INT16_T: {
        const quint16 bits = static_cast<quint16>(
            unsignedWireValue(message, offset, 2));
        return QString::number(signedFromBits<qint16>(bits));
    }
    case MAVLINK_TYPE_UINT32_T:
        return QString::number(static_cast<quint32>(
            unsignedWireValue(message, offset, 4)));
    case MAVLINK_TYPE_INT32_T: {
        const quint32 bits = static_cast<quint32>(
            unsignedWireValue(message, offset, 4));
        return QString::number(signedFromBits<qint32>(bits));
    }
    case MAVLINK_TYPE_UINT64_T:
        return QString::number(unsignedWireValue(message, offset, 8));
    case MAVLINK_TYPE_INT64_T: {
        const quint64 bits = unsignedWireValue(message, offset, 8);
        return QString::number(signedFromBits<qint64>(bits));
    }
    case MAVLINK_TYPE_FLOAT: {
        const quint32 bits = static_cast<quint32>(
            unsignedWireValue(message, offset, 4));
        float value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return QString::number(value, 'g',
                               std::numeric_limits<float>::max_digits10);
    }
    case MAVLINK_TYPE_DOUBLE: {
        const quint64 bits = unsignedWireValue(message, offset, 8);
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return QString::number(value, 'g',
                               std::numeric_limits<double>::max_digits10);
    }
    }
    return QString();
}

QString fieldValue(const mavlink_message_t &message,
                   const mavlink_field_info_t &field)
{
    const unsigned elementSize = wireElementSize(field.type);
    if (elementSize == 0 || field.wire_offset >= MAVLINK_MAX_PAYLOAD_LEN) {
        return QString();
    }

    const unsigned availableElements =
        (MAVLINK_MAX_PAYLOAD_LEN - field.wire_offset) / elementSize;
    const unsigned declaredElements =
        field.array_length == 0 ? 1U : field.array_length;
    const unsigned elementCount =
        std::min(declaredElements, availableElements);
    if (elementCount == 0) {
        return QString();
    }

    if (field.type == MAVLINK_TYPE_CHAR && field.array_length > 0) {
        QByteArray text;
        text.reserve(static_cast<int>(elementCount));
        for (unsigned index = 0; index < elementCount; ++index) {
            const quint8 byte = payloadByte(
                message, field.wire_offset + index);
            if (byte == 0) {
                break;
            }
            text.append(static_cast<char>(byte));
        }
        return QString::fromLatin1(text);
    }

    if (field.array_length == 0) {
        return scalarValue(message, field.type, field.wire_offset);
    }

    QStringList values;
    values.reserve(static_cast<int>(elementCount));
    for (unsigned index = 0; index < elementCount; ++index) {
        values.append(scalarValue(
            message, field.type,
            field.wire_offset + index * elementSize));
    }
    return values.join(QStringLiteral(", "));
}

} // namespace

MAVLinkInspectorPacketStore::MAVLinkInspectorPacketStore(int entryCapacity)
    : m_entryCapacity(qBound(1, entryCapacity, DefaultEntryCapacity))
{
}

bool MAVLinkInspectorPacketStore::add(const mavlink_message_t &message,
                                      quint32 wireBytes,
                                      qint64 monotonicMs)
{
    if (monotonicMs < 0) {
        return false;
    }

    const Key key = keyFor(message);
    const quint64 packed = packedKey(key);
    auto entryIt = m_entries.find(packed);
    if (entryIt == m_entries.end()) {
        if (m_entries.size() >= m_entryCapacity) {
            evictLeastRecentlyUsed();
        }
        entryIt = m_entries.insert(packed, StoredEntry());
    } else if (monotonicMs < entryIt->lastSeenMs) {
        // The caller's monotonic clock regressed. Keep lifetime count/latest
        // semantics, but discard incomparable rate samples for this key.
        entryIt->samples.clear();
    }

    entryIt->latest = message;
    entryIt->lastSeenMs = monotonicMs;
    ++entryIt->totalSeen;
    entryIt->latestWireBytes = wireBytes;
    entryIt->touchSequence = ++m_touchSequence;
    entryIt->samples.append({monotonicMs, wireBytes});
    if (entryIt->samples.size() > RateHistoryPerKey) {
        entryIt->samples.remove(
            0, entryIt->samples.size() - RateHistoryPerKey);
    }
    return true;
}

bool MAVLinkInspectorPacketStore::contains(const Key &key) const
{
    return m_entries.contains(packedKey(key));
}

int MAVLinkInspectorPacketStore::size() const
{
    return m_entries.size();
}

QVector<MAVLinkInspectorPacketStore::Key>
MAVLinkInspectorPacketStore::keys() const
{
    QVector<Key> result;
    result.reserve(m_entries.size());
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        const mavlink_message_t &message = it->latest;
        result.append(keyFor(message));
    }
    std::sort(result.begin(), result.end(), keyLessThan);
    return result;
}

bool MAVLinkInspectorPacketStore::snapshot(const Key &key, qint64 nowMs,
                                           EntrySnapshot *result) const
{
    if (!result) {
        return false;
    }
    const auto entryIt = m_entries.constFind(packedKey(key));
    if (entryIt == m_entries.constEnd()) {
        return false;
    }

    const mavlink_message_info_t *info =
        mavlink_get_message_info_by_id(key.messageId);
    const Rate rate = rateFor(entryIt.value(), nowMs);

    result->key = key;
    result->messageName = info
        ? QString::fromLatin1(info->name)
        : QStringLiteral("UNKNOWN_%1").arg(key.messageId);
    result->latest = entryIt->latest;
    result->lastSeenMs = entryIt->lastSeenMs;
    result->totalSeen = entryIt->totalSeen;
    result->latestWireBytes = entryIt->latestWireBytes;
    result->rateHz = rate.hz;
    result->bytesPerSecond = rate.bytesPerSecond;
    result->knownMessage = info != nullptr;
    return true;
}

QVector<MAVLinkInspectorPacketStore::EntrySnapshot>
MAVLinkInspectorPacketStore::snapshots(qint64 nowMs) const
{
    QVector<EntrySnapshot> result;
    const QVector<Key> orderedKeys = keys();
    result.reserve(orderedKeys.size());
    for (const Key &key : orderedKeys) {
        EntrySnapshot entry;
        if (snapshot(key, nowMs, &entry)) {
            result.append(entry);
        }
    }
    return result;
}

void MAVLinkInspectorPacketStore::clear()
{
    m_entries.clear();
    m_touchSequence = 0;
}

QString MAVLinkInspectorPacketStore::messageName(quint32 messageId)
{
    const mavlink_message_info_t *info =
        mavlink_get_message_info_by_id(messageId);
    return info ? QString::fromLatin1(info->name)
                : QStringLiteral("UNKNOWN_%1").arg(messageId);
}

QVector<MAVLinkInspectorPacketStore::DecodedField>
MAVLinkInspectorPacketStore::decodeFields(const mavlink_message_t &message)
{
    QVector<DecodedField> result;
    const mavlink_message_info_t *info =
        mavlink_get_message_info_by_id(message.msgid);
    if (!info) {
        return result;
    }

    const unsigned fieldCount =
        std::min(info->num_fields, static_cast<unsigned>(MAVLINK_MAX_FIELDS));
    result.reserve(static_cast<int>(fieldCount));
    for (unsigned index = 0; index < fieldCount; ++index) {
        const mavlink_field_info_t &field = info->fields[index];
        result.append({QString::fromLatin1(field.name), fieldTypeName(field),
                       fieldValue(message, field)});
    }
    return result;
}

MAVLinkInspectorPacketStore::Key
MAVLinkInspectorPacketStore::keyFor(const mavlink_message_t &message)
{
    return {message.sysid, message.compid, message.msgid};
}

quint64 MAVLinkInspectorPacketStore::packedKey(const Key &key)
{
    return (static_cast<quint64>(key.systemId) << 40U) |
           (static_cast<quint64>(key.componentId) << 32U) |
           static_cast<quint64>(key.messageId);
}

bool MAVLinkInspectorPacketStore::keyLessThan(const Key &left,
                                              const Key &right)
{
    if (left.systemId != right.systemId) {
        return left.systemId < right.systemId;
    }
    if (left.componentId != right.componentId) {
        return left.componentId < right.componentId;
    }
    return left.messageId < right.messageId;
}

MAVLinkInspectorPacketStore::Rate
MAVLinkInspectorPacketStore::rateFor(const StoredEntry &entry, qint64 nowMs)
{
    Rate result;
    if (entry.samples.isEmpty() || nowMs < 0 || nowMs < entry.lastSeenMs) {
        return result;
    }

    const qint64 windowStart = nowMs - RateWindowMs;
    const qint64 denominatorStart =
        qMax(windowStart, entry.samples.constFirst().timeMs);
    const qint64 elapsedMs = nowMs - denominatorStart;
    if (elapsedMs <= 0) {
        return result;
    }

    quint64 sampleCount = 0;
    quint64 byteCount = 0;
    for (const RateSample &sample : entry.samples) {
        // These strict boundaries intentionally match MP10 PacketInspector.
        if (sample.timeMs > windowStart && sample.timeMs < nowMs) {
            ++sampleCount;
            byteCount += sample.wireBytes;
        }
    }

    const double elapsedSeconds = static_cast<double>(elapsedMs) / 1000.0;
    result.hz = static_cast<double>(sampleCount) / elapsedSeconds;
    result.bytesPerSecond = static_cast<double>(byteCount) / elapsedSeconds;
    return result;
}

void MAVLinkInspectorPacketStore::evictLeastRecentlyUsed()
{
    if (m_entries.isEmpty()) {
        return;
    }

    auto victim = m_entries.begin();
    for (auto it = std::next(victim); it != m_entries.end(); ++it) {
        if (it->touchSequence < victim->touchSequence) {
            victim = it;
        }
    }
    m_entries.erase(victim);
}
