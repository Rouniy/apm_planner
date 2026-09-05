#ifndef MAVLINKINSPECTORPACKETSTORE_H
#define MAVLINKINSPECTORPACKETSTORE_H

#include <cstddef> // Generated message-info tables use offsetof.
#include <mavlink.h>

#include <QHash>
#include <QString>
#include <QVector>

/**
 * Transport-independent packet storage and field decoding for the MAVLink
 * Inspector. The caller owns source selection and pause/resume policy; this
 * class distinguishes packets only by their exact system/component/message
 * identity.
 *
 * Timestamps must come from one monotonic clock. A timestamp which goes
 * backwards resets that key's rate history before the new sample is accepted,
 * so stale samples can never produce a misleading rate after clock rollback.
 */
class MAVLinkInspectorPacketStore final
{
public:
    static constexpr int DefaultEntryCapacity = 4096;
    static constexpr int RateHistoryPerKey = 200;
    static constexpr qint64 RateWindowMs = 3000;

    struct Key
    {
        quint8 systemId = 0;
        quint8 componentId = 0;
        quint32 messageId = 0;

        bool operator==(const Key &other) const
        {
            return systemId == other.systemId &&
                   componentId == other.componentId &&
                   messageId == other.messageId;
        }
    };

    struct EntrySnapshot
    {
        Key key;
        QString messageName;
        mavlink_message_t latest{};
        qint64 lastSeenMs = 0;
        quint64 totalSeen = 0;
        quint32 latestWireBytes = 0;
        double rateHz = 0.0;
        double bytesPerSecond = 0.0;
        bool knownMessage = false;
    };

    struct DecodedField
    {
        QString name;
        QString type;
        QString value;
    };

    explicit MAVLinkInspectorPacketStore(
        int entryCapacity = DefaultEntryCapacity);

    // Returns false only for an invalid (negative) monotonic timestamp.
    bool add(const mavlink_message_t &message, quint32 wireBytes,
             qint64 monotonicMs);

    bool contains(const Key &key) const;
    int size() const;
    QVector<Key> keys() const;
    bool snapshot(const Key &key, qint64 nowMs, EntrySnapshot *result) const;
    QVector<EntrySnapshot> snapshots(qint64 nowMs) const;
    void clear();

    static QString messageName(quint32 messageId);
    static QVector<DecodedField> decodeFields(
        const mavlink_message_t &message);

private:
    struct RateSample
    {
        qint64 timeMs = 0;
        quint32 wireBytes = 0;
    };

    struct StoredEntry
    {
        mavlink_message_t latest{};
        QVector<RateSample> samples;
        qint64 lastSeenMs = 0;
        quint64 totalSeen = 0;
        quint32 latestWireBytes = 0;
        quint64 touchSequence = 0;
    };

    struct Rate
    {
        double hz = 0.0;
        double bytesPerSecond = 0.0;
    };

    static Key keyFor(const mavlink_message_t &message);
    static quint64 packedKey(const Key &key);
    static bool keyLessThan(const Key &left, const Key &right);
    static Rate rateFor(const StoredEntry &entry, qint64 nowMs);

    void evictLeastRecentlyUsed();

    int m_entryCapacity = DefaultEntryCapacity;
    quint64 m_touchSequence = 0;
    QHash<quint64, StoredEntry> m_entries;
};

#endif // MAVLINKINSPECTORPACKETSTORE_H
