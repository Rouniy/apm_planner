#include "MAVLinkInspectorPacketStore.h"

#include <QtTest/QTest>

#include <cmath>
#include <cstring>

namespace {

mavlink_message_t message(quint8 systemId, quint8 componentId,
                          quint32 messageId, quint8 payloadLength = 0)
{
    mavlink_message_t result{};
    result.sysid = systemId;
    result.compid = componentId;
    result.msgid = messageId;
    result.len = payloadLength;
    return result;
}

QString decodedValue(
    const QVector<MAVLinkInspectorPacketStore::DecodedField> &fields,
    const QString &name)
{
    for (const MAVLinkInspectorPacketStore::DecodedField &field : fields) {
        if (field.name == name) {
            return field.value;
        }
    }
    return QString();
}

QString decodedType(
    const QVector<MAVLinkInspectorPacketStore::DecodedField> &fields,
    const QString &name)
{
    for (const MAVLinkInspectorPacketStore::DecodedField &field : fields) {
        if (field.name == name) {
            return field.type;
        }
    }
    return QString();
}

} // namespace

class MAVLinkInspectorPacketStoreTest final : public QObject
{
    Q_OBJECT

private slots:
    void keepsExactKeysAndLatestCopy();
    void matchesMp10RateWindowBoundaries();
    void boundsRateHistoryAtTwoHundredSamples();
    void evictsLeastRecentlyUsedEntry();
    void resetsOnlyRegressedKeyRateHistory();
    void decodesExactUnsignedValuesWithoutMutation();
    void decodesUnalignedAndTrimmedFieldsSafely();
    void preservesFullLengthCharacterArrays();
    void neverReadsChecksumAsCharacterData();
    void handlesUnknownMessageIds();
};

void MAVLinkInspectorPacketStoreTest::keepsExactKeysAndLatestCopy()
{
    MAVLinkInspectorPacketStore store;
    mavlink_message_t first = message(1, 2, MAVLINK_MSG_ID_HEARTBEAT, 1);
    _MAV_PAYLOAD_NON_CONST(&first)[0] = 17;
    QVERIFY(store.add(first, 21, 100));

    mavlink_message_t replacement = first;
    _MAV_PAYLOAD_NON_CONST(&replacement)[0] = 42;
    QVERIFY(store.add(replacement, 22, 200));
    _MAV_PAYLOAD_NON_CONST(&replacement)[0] = 99;

    QVERIFY(store.add(message(1, 3, MAVLINK_MSG_ID_HEARTBEAT), 20, 210));
    QVERIFY(store.add(message(2, 2, MAVLINK_MSG_ID_HEARTBEAT), 20, 220));
    QVERIFY(store.add(message(1, 2, MAVLINK_MSG_ID_ATTITUDE), 20, 230));
    QCOMPARE(store.size(), 4);

    MAVLinkInspectorPacketStore::EntrySnapshot snapshot;
    const MAVLinkInspectorPacketStore::Key key{
        1, 2, MAVLINK_MSG_ID_HEARTBEAT};
    QVERIFY(store.snapshot(key, 300, &snapshot));
    QCOMPARE(snapshot.key.systemId, key.systemId);
    QCOMPARE(snapshot.key.componentId, key.componentId);
    QCOMPARE(snapshot.key.messageId, key.messageId);
    QCOMPARE(snapshot.totalSeen, quint64(2));
    QCOMPARE(snapshot.latestWireBytes, quint32(22));
    QCOMPARE(static_cast<quint8>(_MAV_PAYLOAD(&snapshot.latest)[0]),
             quint8(42));
    QCOMPARE(snapshot.messageName, QStringLiteral("HEARTBEAT"));
    QVERIFY(snapshot.knownMessage);
}

void MAVLinkInspectorPacketStoreTest::matchesMp10RateWindowBoundaries()
{
    MAVLinkInspectorPacketStore store;
    const mavlink_message_t packet = message(1, 1, MAVLINK_MSG_ID_HEARTBEAT);
    const qint64 times[] = {0, 1500, 2000, 3000, 4000, 4500};
    const quint32 sizes[] = {10, 20, 30, 40, 50, 60};
    for (int index = 0; index < 6; ++index) {
        QVERIFY(store.add(packet, sizes[index], times[index]));
    }

    MAVLinkInspectorPacketStore::EntrySnapshot snapshot;
    QVERIFY(store.snapshot({1, 1, MAVLINK_MSG_ID_HEARTBEAT}, 4500,
                           &snapshot));
    QCOMPARE(snapshot.rateHz, 1.0);
    QCOMPARE(snapshot.bytesPerSecond, 40.0);
}

void MAVLinkInspectorPacketStoreTest::boundsRateHistoryAtTwoHundredSamples()
{
    MAVLinkInspectorPacketStore store;
    const mavlink_message_t packet = message(1, 1, MAVLINK_MSG_ID_HEARTBEAT);
    for (int index = 0; index <= 200; ++index) {
        QVERIFY(store.add(packet, 2, index * 10));
    }

    MAVLinkInspectorPacketStore::EntrySnapshot snapshot;
    QVERIFY(store.snapshot({1, 1, MAVLINK_MSG_ID_HEARTBEAT}, 2100,
                           &snapshot));
    QVERIFY(std::abs(snapshot.rateHz - (200.0 / 2.09)) < 1.0e-9);
    QVERIFY(std::abs(snapshot.bytesPerSecond - (400.0 / 2.09)) < 1.0e-9);
    QCOMPARE(snapshot.totalSeen, quint64(201));
}

void MAVLinkInspectorPacketStoreTest::evictsLeastRecentlyUsedEntry()
{
    MAVLinkInspectorPacketStore store(2);
    const mavlink_message_t a = message(1, 1, 1);
    const mavlink_message_t b = message(1, 1, 2);
    const mavlink_message_t c = message(1, 1, 3);
    QVERIFY(store.add(a, 1, 100));
    QVERIFY(store.add(b, 1, 200));
    QVERIFY(store.add(a, 1, 300));
    QVERIFY(store.add(c, 1, 400));

    QVERIFY(store.contains({1, 1, 1}));
    QVERIFY(!store.contains({1, 1, 2}));
    QVERIFY(store.contains({1, 1, 3}));
    QCOMPARE(store.size(), 2);
}

void MAVLinkInspectorPacketStoreTest::resetsOnlyRegressedKeyRateHistory()
{
    MAVLinkInspectorPacketStore store;
    const mavlink_message_t a = message(1, 1, 1);
    const mavlink_message_t b = message(1, 1, 2);
    QVERIFY(store.add(a, 10, 1000));
    QVERIFY(store.add(a, 10, 2000));
    QVERIFY(store.add(b, 10, 1000));
    QVERIFY(store.add(b, 10, 2000));
    QVERIFY(store.add(a, 10, 1500));

    MAVLinkInspectorPacketStore::EntrySnapshot aSnapshot;
    MAVLinkInspectorPacketStore::EntrySnapshot bSnapshot;
    QVERIFY(store.snapshot({1, 1, 1}, 2500, &aSnapshot));
    QVERIFY(store.snapshot({1, 1, 2}, 2500, &bSnapshot));
    QCOMPARE(aSnapshot.rateHz, 1.0);
    QCOMPARE(aSnapshot.totalSeen, quint64(3));
    QVERIFY(std::abs(bSnapshot.rateHz - (2.0 / 1.5)) < 1.0e-9);
    QCOMPARE(bSnapshot.totalSeen, quint64(2));

    const int oldSize = store.size();
    QVERIFY(!store.add(a, 10, -1));
    QCOMPARE(store.size(), oldSize);
    QVERIFY(store.snapshot({1, 1, 1}, 2500, &aSnapshot));
    QCOMPARE(aSnapshot.totalSeen, quint64(3));
}

void MAVLinkInspectorPacketStoreTest::decodesExactUnsignedValuesWithoutMutation()
{
    mavlink_message_t packet = message(
        1, 1, MAVLINK_MSG_ID_SYSTEM_TIME, MAVLINK_MSG_ID_SYSTEM_TIME_LEN);
    std::memset(_MAV_PAYLOAD_NON_CONST(&packet), 0xff,
                MAVLINK_MSG_ID_SYSTEM_TIME_LEN);
    const mavlink_message_t before = packet;

    const auto fields = MAVLinkInspectorPacketStore::decodeFields(packet);
    QCOMPARE(decodedValue(fields, QStringLiteral("time_unix_usec")),
             QStringLiteral("18446744073709551615"));
    QCOMPARE(decodedValue(fields, QStringLiteral("time_boot_ms")),
             QStringLiteral("4294967295"));
    QCOMPARE(decodedType(fields, QStringLiteral("time_unix_usec")),
             QStringLiteral("uint64_t"));
    QCOMPARE(std::memcmp(&packet, &before, sizeof(packet)), 0);
}

void MAVLinkInspectorPacketStoreTest::decodesUnalignedAndTrimmedFieldsSafely()
{
    mavlink_message_t packet = message(
        1, 1, MAVLINK_MSG_ID_SMART_BATTERY_INFO, 94);
    auto *payload = reinterpret_cast<quint8 *>(
        _MAV_PAYLOAD_NON_CONST(&packet));
    payload[90] = 0x78;
    payload[91] = 0x56;
    payload[92] = 0x34;
    payload[93] = 0x12;

    auto fields = MAVLinkInspectorPacketStore::decodeFields(packet);
    QCOMPARE(decodedValue(fields,
                          QStringLiteral("discharge_maximum_current")),
             QStringLiteral("305419896"));

    packet.len = 90;
    fields = MAVLinkInspectorPacketStore::decodeFields(packet);
    QCOMPARE(decodedValue(fields,
                          QStringLiteral("discharge_maximum_current")),
             QStringLiteral("0"));
}

void MAVLinkInspectorPacketStoreTest::preservesFullLengthCharacterArrays()
{
    mavlink_message_t packet = message(
        1, 1, MAVLINK_MSG_ID_PARAM_VALUE, MAVLINK_MSG_ID_PARAM_VALUE_LEN);
    const QByteArray fullId("SIXTEEN_CHAR_ID!", 16);
    std::memcpy(_MAV_PAYLOAD_NON_CONST(&packet) + 8,
                fullId.constData(), 16);

    const auto fields = MAVLinkInspectorPacketStore::decodeFields(packet);
    QCOMPARE(decodedValue(fields, QStringLiteral("param_id")),
             QString::fromLatin1(fullId));
    QCOMPARE(decodedType(fields, QStringLiteral("param_id")),
             QStringLiteral("char[16]"));
}

void MAVLinkInspectorPacketStoreTest::neverReadsChecksumAsCharacterData()
{
    mavlink_message_t packet = message(1, 1, MAVLINK_MSG_ID_STATUSTEXT, 6);
    auto *payload = _MAV_PAYLOAD_NON_CONST(&packet);
    payload[0] = static_cast<char>(MAV_SEVERITY_INFO);
    std::memcpy(payload + 1, "ABCDE", 5);
    payload[6] = 'Z';
    const mavlink_message_t before = packet;

    const auto fields = MAVLinkInspectorPacketStore::decodeFields(packet);
    QCOMPARE(decodedValue(fields, QStringLiteral("text")),
             QStringLiteral("ABCDE"));
    QCOMPARE(decodedValue(fields, QStringLiteral("id")), QStringLiteral("0"));
    QCOMPARE(std::memcmp(&packet, &before, sizeof(packet)), 0);
}

void MAVLinkInspectorPacketStoreTest::handlesUnknownMessageIds()
{
    const mavlink_message_t packet = message(9, 8, 0x00fffffeU, 1);
    QCOMPARE(MAVLinkInspectorPacketStore::messageName(packet.msgid),
             QStringLiteral("UNKNOWN_16777214"));
    QVERIFY(MAVLinkInspectorPacketStore::decodeFields(packet).isEmpty());

    MAVLinkInspectorPacketStore store;
    QVERIFY(store.add(packet, 13, 10));
    MAVLinkInspectorPacketStore::EntrySnapshot snapshot;
    QVERIFY(store.snapshot({9, 8, 0x00fffffeU}, 20, &snapshot));
    QVERIFY(!snapshot.knownMessage);
    QCOMPARE(snapshot.messageName, QStringLiteral("UNKNOWN_16777214"));
}

QTEST_APPLESS_MAIN(MAVLinkInspectorPacketStoreTest)
#include "test_mavlinkinspectorpacketstore.moc"
