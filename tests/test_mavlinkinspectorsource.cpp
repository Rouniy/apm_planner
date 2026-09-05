#include <QtTest>

#include "ui/MAVLinkInspectorTrafficSource.h"

#include <QPointer>
#include <QStringList>

class MAVLinkInspectorTrafficSourceTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void liveSourceFiltersIdentityAndEpoch();
    void outboundSourceUsesPinnedLiveIdentity();
    void disconnectedBindingActivatesOnFirstSession();
    void liveDisconnectFreezesAndReconnectResets();
    void liveRemovalIsTerminal();
    void liveObjectDestructionIsTerminal();
    void replaySourceFiltersGenerationAndEnds();
    void bindingIsOneShotAndKindSpecific();
    void activeTokenTracksAcceptedSession();
    void reentrantStatusChangeBlocksDelivery();
    void reentrantResetChangeWins();
};

void MAVLinkInspectorTrafficSourceTest::initTestCase()
{
    qRegisterMetaType<mavlink_message_t>("mavlink_message_t");
}

void MAVLinkInspectorTrafficSourceTest::disconnectedBindingActivatesOnFirstSession()
{
    int deliveries = 0;
    int resets = 0;
    QObject selectedLink;
    QObject otherLink;
    MAVLinkInspectorTrafficSource source;
    connect(&source, &MAVLinkInspectorTrafficSource::messageReceived,
            &source, [&deliveries](const mavlink_message_t &) {
                ++deliveries;
            });
    connect(&source, &MAVLinkInspectorTrafficSource::sourceReset,
            &source, [&resets]() {
                ++resets;
            });

    QVERIFY(source.bindLive(&selectedLink, 3, 0,
                            QStringLiteral("Configured serial")));
    QVERIFY(source.status().contains(QStringLiteral("is disconnected")));
    mavlink_message_t message{};
    source.observeLive(&selectedLink, 3, 0, message);
    source.observeLive(&selectedLink, 3, 1, message);
    source.beginLiveSession(&otherLink, 3, 1);
    source.beginLiveSession(&selectedLink, 4, 1);
    source.beginLiveSession(&selectedLink, 3, 0);
    QCOMPARE(deliveries, 0);
    QCOMPARE(resets, 0);

    source.beginLiveSession(&selectedLink, 3, 1);
    QCOMPARE(resets, 1);
    QVERIFY(source.status().startsWith(QStringLiteral("Waiting")));
    source.observeLive(&selectedLink, 3, 1, message);
    QCOMPARE(deliveries, 1);
}

void MAVLinkInspectorTrafficSourceTest::liveSourceFiltersIdentityAndEpoch()
{
    QList<quint32> deliveredIds;
    QStringList statuses;
    QObject selectedLink;
    QObject otherLink;
    MAVLinkInspectorTrafficSource source;
    connect(&source, &MAVLinkInspectorTrafficSource::messageReceived,
            &source, [&deliveredIds](const mavlink_message_t &message) {
                deliveredIds.append(message.msgid);
            });
    connect(&source, &MAVLinkInspectorTrafficSource::statusChanged,
            &source, [&statuses](const QString &status) {
                statuses.append(status);
            });

    QCOMPARE(source.status(), QStringLiteral("No MAVLink source selected."));
    QVERIFY(source.bindLive(&selectedLink, 7, 41,
                            QStringLiteral("Telemetry A")));
    QVERIFY(source.status().contains(QStringLiteral("Telemetry A")));
    QCOMPARE(statuses.size(), 1);

    mavlink_message_t accepted{};
    accepted.msgid = 30;
    mavlink_message_t rejected{};
    rejected.msgid = 33;
    source.observeLive(&otherLink, 7, 41, rejected);
    source.observeLive(&selectedLink, 8, 41, rejected);
    source.observeLive(&selectedLink, 7, 40, rejected);
    source.observeReplay(41, rejected);
    QCOMPARE(deliveredIds.size(), 0);
    QCOMPARE(statuses.size(), 1);

    source.observeLive(&selectedLink, 7, 41, accepted);
    QCOMPARE(deliveredIds, QList<quint32>({30}));
    QVERIFY(source.status().startsWith(QStringLiteral("Receiving")));
    QCOMPARE(statuses.size(), 2);

    accepted.msgid = 74;
    source.observeLive(&selectedLink, 7, 41, accepted);
    QCOMPARE(deliveredIds, QList<quint32>({30, 74}));
    QCOMPARE(statuses.size(), 2);
}

void MAVLinkInspectorTrafficSourceTest::outboundSourceUsesPinnedLiveIdentity()
{
    QList<quint32> incomingIds;
    QList<quint32> outgoingIds;
    QObject selectedLink;
    QObject otherLink;
    MAVLinkInspectorTrafficSource source;
    connect(&source, &MAVLinkInspectorTrafficSource::messageReceived,
            &source, [&incomingIds](const mavlink_message_t &message) {
                incomingIds.append(message.msgid);
            });
    connect(&source, &MAVLinkInspectorTrafficSource::outboundMessageReceived,
            &source, [&outgoingIds](const mavlink_message_t &message) {
                outgoingIds.append(message.msgid);
            });

    QVERIFY(source.bindLive(&selectedLink, 14, 90,
                            QStringLiteral("Pinned")));
    mavlink_message_t outgoing{};
    outgoing.msgid = 76;
    source.observeOutbound(&otherLink, 14, 90, outgoing);
    source.observeOutbound(&selectedLink, 13, 90, outgoing);
    source.observeOutbound(&selectedLink, 14, 89, outgoing);
    QCOMPARE(outgoingIds.size(), 0);

    source.observeOutbound(&selectedLink, 14, 90, outgoing);
    QCOMPARE(outgoingIds, QList<quint32>({76}));
    QCOMPARE(incomingIds.size(), 0);

    mavlink_message_t incoming{};
    incoming.msgid = 77;
    source.observeLive(&selectedLink, 14, 90, incoming);
    QCOMPARE(incomingIds, QList<quint32>({77}));
    QCOMPARE(outgoingIds, QList<quint32>({76}));

    int replayOutgoing = 0;
    MAVLinkInspectorTrafficSource replay;
    connect(&replay,
            &MAVLinkInspectorTrafficSource::outboundMessageReceived,
            &replay, [&replayOutgoing](const mavlink_message_t &) {
                ++replayOutgoing;
            });
    QVERIFY(replay.bindReplay(90, QStringLiteral("Replay")));
    replay.observeOutbound(&selectedLink, 14, 90, outgoing);
    QCOMPARE(replayOutgoing, 0);
}

void MAVLinkInspectorTrafficSourceTest::liveDisconnectFreezesAndReconnectResets()
{
    int deliveries = 0;
    int resets = 0;
    QObject selectedLink;
    QObject otherLink;
    MAVLinkInspectorTrafficSource source;
    connect(&source, &MAVLinkInspectorTrafficSource::messageReceived,
            &source, [&deliveries](const mavlink_message_t &) {
                ++deliveries;
            });
    connect(&source, &MAVLinkInspectorTrafficSource::sourceReset,
            &source, [&resets]() {
                ++resets;
            });

    QVERIFY(source.bindLive(&selectedLink, 12, 100,
                            QStringLiteral("Serial 1")));
    mavlink_message_t message{};
    message.msgid = 0;
    source.observeLive(&selectedLink, 12, 100, message);
    QCOMPARE(deliveries, 1);

    source.endLiveSession(11, 100);
    source.endLiveSession(12, 99);
    source.observeLive(&selectedLink, 12, 100, message);
    QCOMPARE(deliveries, 2);

    source.endLiveSession(12, 100);
    QVERIFY(source.status().contains(QStringLiteral("disconnected")));
    source.observeLive(&selectedLink, 12, 100, message);
    QCOMPARE(deliveries, 2);

    source.beginLiveSession(&otherLink, 12, 101);
    source.beginLiveSession(&selectedLink, 11, 101);
    source.beginLiveSession(&selectedLink, 12, 100);
    QCOMPARE(resets, 0);
    source.observeLive(&selectedLink, 12, 101, message);
    QCOMPARE(deliveries, 2);

    source.beginLiveSession(&selectedLink, 12, 101);
    QCOMPARE(resets, 1);
    QVERIFY(source.status().startsWith(QStringLiteral("Waiting")));
    source.observeLive(&selectedLink, 12, 100, message);
    QCOMPARE(deliveries, 2);
    source.observeLive(&selectedLink, 12, 101, message);
    QCOMPARE(deliveries, 3);

    // A newer epoch is also a reset if its end notification was lost.  A
    // delayed end from the previous epoch must not freeze the replacement.
    source.beginLiveSession(&selectedLink, 12, 102);
    QCOMPARE(resets, 2);
    source.endLiveSession(12, 101);
    source.observeLive(&selectedLink, 12, 102, message);
    QCOMPARE(deliveries, 4);
}

void MAVLinkInspectorTrafficSourceTest::liveRemovalIsTerminal()
{
    int deliveries = 0;
    int resets = 0;
    QObject selectedLink;
    MAVLinkInspectorTrafficSource source;
    connect(&source, &MAVLinkInspectorTrafficSource::messageReceived,
            &source, [&deliveries](const mavlink_message_t &) {
                ++deliveries;
            });
    connect(&source, &MAVLinkInspectorTrafficSource::sourceReset,
            &source, [&resets]() {
                ++resets;
            });

    QVERIFY(source.bindLive(&selectedLink, 9, 1, QStringLiteral("UDP")));
    source.removeLiveLink(8);
    mavlink_message_t message{};
    source.observeLive(&selectedLink, 9, 1, message);
    QCOMPARE(deliveries, 1);

    source.removeLiveLink(9);
    QVERIFY(source.status().contains(QStringLiteral("no longer available")));
    source.observeLive(&selectedLink, 9, 1, message);
    source.beginLiveSession(&selectedLink, 9, 2);
    source.endLiveSession(9, 1);
    QCOMPARE(deliveries, 1);
    QCOMPARE(resets, 0);
    QVERIFY(!source.bindReplay(2, QStringLiteral("Replay")));
}

void MAVLinkInspectorTrafficSourceTest::liveObjectDestructionIsTerminal()
{
    int deliveries = 0;
    int resets = 0;
    MAVLinkInspectorTrafficSource source;
    auto *selectedLink = new QObject;
    connect(&source, &MAVLinkInspectorTrafficSource::messageReceived,
            &source, [&deliveries](const mavlink_message_t &) {
                ++deliveries;
            });
    connect(&source, &MAVLinkInspectorTrafficSource::sourceReset,
            &source, [&resets]() {
                ++resets;
            });

    QVERIFY(source.bindLive(selectedLink, 6, 4, QStringLiteral("TCP")));
    delete selectedLink;
    QVERIFY(source.status().contains(QStringLiteral("was destroyed")));

    QObject replacement;
    mavlink_message_t message{};
    source.beginLiveSession(&replacement, 6, 5);
    source.observeLive(&replacement, 6, 5, message);
    QCOMPARE(deliveries, 0);
    QCOMPARE(resets, 0);
    QVERIFY(!source.bindLive(&replacement, 6, 5,
                             QStringLiteral("Replacement")));
}

void MAVLinkInspectorTrafficSourceTest::replaySourceFiltersGenerationAndEnds()
{
    QList<quint32> deliveredIds;
    int resets = 0;
    MAVLinkInspectorTrafficSource source;
    connect(&source, &MAVLinkInspectorTrafficSource::messageReceived,
            &source, [&deliveredIds](const mavlink_message_t &message) {
                deliveredIds.append(message.msgid);
            });
    connect(&source, &MAVLinkInspectorTrafficSource::sourceReset,
            &source, [&resets]() {
                ++resets;
            });

    QVERIFY(source.bindReplay(500, QStringLiteral("flight.tlog")));
    mavlink_message_t message{};
    message.msgid = 147;
    source.observeReplay(499, message);
    source.observeLive(nullptr, 0, 500, message);
    QCOMPARE(deliveredIds.size(), 0);

    source.observeReplay(500, message);
    QCOMPARE(deliveredIds, QList<quint32>({147}));
    QVERIFY(source.status().startsWith(QStringLiteral("Receiving")));
    source.endReplay(499);
    source.observeReplay(500, message);
    QCOMPARE(deliveredIds.size(), 2);

    source.endReplay(500);
    QVERIFY(source.status().contains(QStringLiteral("ended")));
    source.observeReplay(500, message);
    QCOMPARE(deliveredIds.size(), 2);
    QCOMPARE(resets, 0);
    QVERIFY(!source.bindLive(new QObject(&source), 1, 1,
                             QStringLiteral("Live")));
}

void MAVLinkInspectorTrafficSourceTest::bindingIsOneShotAndKindSpecific()
{
    QObject link;
    MAVLinkInspectorTrafficSource invalid;
    QVERIFY(!invalid.bindLive(nullptr, 1, 1, QStringLiteral("bad")));
    QVERIFY(!invalid.bindLive(&link, -1, 1, QStringLiteral("bad")));
    QCOMPARE(invalid.status(),
             QStringLiteral("No MAVLink source selected."));

    MAVLinkInspectorTrafficSource live;
    QCOMPARE(live.status(), QStringLiteral("No MAVLink source selected."));
    QVERIFY(live.bindLive(&link, 1, 2, QString()));
    QVERIFY(live.status().contains(QStringLiteral("Link 1")));
    QVERIFY(!live.bindLive(&link, 1, 2, QStringLiteral("again")));
    QVERIFY(!live.bindReplay(2, QStringLiteral("wrong kind")));

    MAVLinkInspectorTrafficSource replay;
    QVERIFY(!replay.bindReplay(0, QStringLiteral("bad")));
    QVERIFY(replay.bindReplay(1, QString()));
    QVERIFY(replay.status().contains(QStringLiteral("Telemetry replay")));
    QVERIFY(!replay.bindReplay(2, QStringLiteral("again")));
    replay.endLiveSession(1, 1);
    replay.removeLiveLink(1);
    QVERIFY(replay.status().startsWith(QStringLiteral("Waiting")));
}

void MAVLinkInspectorTrafficSourceTest::activeTokenTracksAcceptedSession()
{
    QObject link;
    MAVLinkInspectorTrafficSource live;
    QCOMPARE(live.activeToken(), quint64(0));
    QVERIFY(live.bindLive(&link, 5, 0, QStringLiteral("Serial")));
    QCOMPARE(live.activeToken(), quint64(0));

    live.beginLiveSession(&link, 5, 70);
    QCOMPARE(live.activeToken(), quint64(70));
    live.endLiveSession(5, 69);
    QCOMPARE(live.activeToken(), quint64(70));
    live.endLiveSession(5, 70);
    QCOMPARE(live.activeToken(), quint64(0));

    live.beginLiveSession(&link, 5, 71);
    QCOMPARE(live.activeToken(), quint64(71));
    live.removeLiveLink(5);
    QCOMPARE(live.activeToken(), quint64(0));

    MAVLinkInspectorTrafficSource replay;
    QCOMPARE(replay.activeToken(), quint64(0));
    QVERIFY(replay.bindReplay(800, QStringLiteral("Log")));
    QCOMPARE(replay.activeToken(), quint64(800));
    replay.endReplay(799);
    QCOMPARE(replay.activeToken(), quint64(800));
    replay.endReplay(800);
    QCOMPARE(replay.activeToken(), quint64(0));
}

void MAVLinkInspectorTrafficSourceTest::reentrantStatusChangeBlocksDelivery()
{
    int liveDeliveries = 0;
    QObject link;
    MAVLinkInspectorTrafficSource live;
    QVERIFY(live.bindLive(&link, 4, 20, QStringLiteral("Live")));
    connect(&live, &MAVLinkInspectorTrafficSource::messageReceived,
            &live, [&liveDeliveries](const mavlink_message_t &) {
                ++liveDeliveries;
            });
    connect(&live, &MAVLinkInspectorTrafficSource::statusChanged,
            &live, [&live](const QString &status) {
                if (status.startsWith(QStringLiteral("Receiving"))) {
                    live.endLiveSession(4, 20);
                }
            });

    mavlink_message_t message{};
    live.observeLive(&link, 4, 20, message);
    QCOMPARE(liveDeliveries, 0);
    QVERIFY(live.status().contains(QStringLiteral("disconnected")));

    int replayDeliveries = 0;
    MAVLinkInspectorTrafficSource replay;
    QVERIFY(replay.bindReplay(30, QStringLiteral("Replay")));
    connect(&replay, &MAVLinkInspectorTrafficSource::messageReceived,
            &replay, [&replayDeliveries](const mavlink_message_t &) {
                ++replayDeliveries;
            });
    connect(&replay, &MAVLinkInspectorTrafficSource::statusChanged,
            &replay, [&replay](const QString &status) {
                if (status.startsWith(QStringLiteral("Receiving"))) {
                    replay.endReplay(30);
                }
            });
    replay.observeReplay(30, message);
    QCOMPARE(replayDeliveries, 0);
    QVERIFY(replay.status().contains(QStringLiteral("ended")));

    int outboundDeliveries = 0;
    QObject outboundLink;
    MAVLinkInspectorTrafficSource outbound;
    QVERIFY(outbound.bindLive(&outboundLink, 6, 40,
                              QStringLiteral("Outbound")));
    connect(&outbound,
            &MAVLinkInspectorTrafficSource::outboundMessageReceived,
            &outbound, [&outboundDeliveries](const mavlink_message_t &) {
                ++outboundDeliveries;
            });
    connect(&outbound, &MAVLinkInspectorTrafficSource::statusChanged,
            &outbound, [&outbound](const QString &status) {
                if (status.startsWith(QStringLiteral("Receiving"))) {
                    outbound.endLiveSession(6, 40);
                }
            });
    outbound.observeOutbound(&outboundLink, 6, 40, message);
    QCOMPARE(outboundDeliveries, 0);
    QCOMPARE(outbound.activeToken(), quint64(0));
}

void MAVLinkInspectorTrafficSourceTest::reentrantResetChangeWins()
{
    int resets = 0;
    int deliveries = 0;
    mavlink_message_t nestedMessage{};
    nestedMessage.msgid = 111;
    QObject link;
    MAVLinkInspectorTrafficSource source;
    QVERIFY(source.bindLive(&link, 2, 80, QStringLiteral("Radio")));
    source.endLiveSession(2, 80);
    connect(&source, &MAVLinkInspectorTrafficSource::sourceReset,
            &source, [&source, &link, &nestedMessage, &resets]() {
                ++resets;
                source.observeLive(&link, 2, 81, nestedMessage);
                source.endLiveSession(2, 81);
            });
    connect(&source, &MAVLinkInspectorTrafficSource::messageReceived,
            &source, [&deliveries](const mavlink_message_t &) {
                ++deliveries;
            });

    source.beginLiveSession(&link, 2, 81);
    QCOMPARE(resets, 1);
    QVERIFY(source.status().contains(QStringLiteral("disconnected")));
    mavlink_message_t message{};
    source.observeLive(&link, 2, 81, message);
    QCOMPARE(deliveries, 0);
}

QTEST_MAIN(MAVLinkInspectorTrafficSourceTest)
#include "test_mavlinkinspectorsource.moc"
