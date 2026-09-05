#include "comm/UdpPeerState.h"

#include <QtTest/QTest>

#include <limits>

class UdpPeerStateTest final : public QObject
{
    Q_OBJECT

private slots:
    void revisionsTrackOnlyRealPeerChanges();
    void strictEnqueueRejectsWrongRevision();
    void queuedDatagramNeverMovesToReplacementPeer();
    void lifecycleRevisionInvalidatesQueuedTraffic();
    void revisionExhaustionFailsClosedWithoutWrapping();
};

void UdpPeerStateTest::revisionsTrackOnlyRealPeerChanges()
{
    UdpPeerState state;
    QCOMPARE(state.snapshot().revision, quint64(0));
    QVERIFY(state.snapshot().hosts.isEmpty());

    const QHostAddress first(QStringLiteral("127.0.0.1"));
    UdpPeerUpdate update = state.upsertPeer(first, 14550);
    QVERIFY(update.accepted);
    QVERIFY(update.changed);
    QCOMPARE(update.revision, quint64(1));

    update = state.upsertPeer(first, 14550);
    QVERIFY(update.accepted);
    QVERIFY(!update.changed);
    QCOMPARE(update.revision, quint64(1));

    update = state.upsertPeer(first, 14551);
    QVERIFY(update.accepted);
    QVERIFY(update.changed);
    QCOMPARE(update.revision, quint64(2));
    UdpPeerSnapshot snapshot = state.snapshot();
    QCOMPARE(snapshot.hosts.size(), 1);
    QCOMPARE(snapshot.hosts.first(), first);
    QCOMPARE(snapshot.ports.first(), quint16(14551));

    update = state.removePeer(QHostAddress(QStringLiteral("127.0.0.2")));
    QVERIFY(update.accepted);
    QVERIFY(!update.changed);
    QCOMPARE(update.revision, quint64(2));

    update = state.removePeer(first);
    QVERIFY(update.accepted);
    QVERIFY(update.changed);
    QCOMPARE(update.revision, quint64(3));
    QVERIFY(state.snapshot().hosts.isEmpty());
}

void UdpPeerStateTest::strictEnqueueRejectsWrongRevision()
{
    UdpPeerState state;
    const QHostAddress peer(QStringLiteral("192.0.2.10"));
    QCOMPARE(state.upsertPeer(peer, 14550).revision, quint64(1));

    QVERIFY(!state.enqueueForRevision(QByteArrayLiteral("wrong"), 0));
    QCOMPARE(state.queuedDatagramCount(), 0);
    QVERIFY(state.enqueueForRevision(QByteArrayLiteral("right"), 1));
    QCOMPARE(state.queuedDatagramCount(), 1);

    const std::optional<UdpPeerDatagram> datagram = state.takeNextCurrent();
    QVERIFY(datagram);
    QCOMPARE(datagram->bytes, QByteArrayLiteral("right"));
    QCOMPARE(datagram->peers.revision, quint64(1));
    QCOMPARE(datagram->peers.hosts.first(), peer);
    QCOMPARE(datagram->peers.ports.first(), quint16(14550));
}

void UdpPeerStateTest::queuedDatagramNeverMovesToReplacementPeer()
{
    UdpPeerState state;
    const QHostAddress peer(QStringLiteral("198.51.100.20"));
    QCOMPARE(state.upsertPeer(peer, 14550).revision, quint64(1));
    QVERIFY(state.enqueueLatest(QByteArrayLiteral("old-session")));

    // Same IP with a new source port is a new complete peer identity.
    QCOMPARE(state.upsertPeer(peer, 15550).revision, quint64(2));
    QVERIFY(!state.takeNextCurrent());
    QCOMPARE(state.queuedDatagramCount(), 0);

    QVERIFY(!state.enqueueForRevision(QByteArrayLiteral("stale"), 1));
    QVERIFY(state.enqueueForRevision(QByteArrayLiteral("new-session"), 2));
    const std::optional<UdpPeerDatagram> datagram = state.takeNextCurrent();
    QVERIFY(datagram);
    QCOMPARE(datagram->bytes, QByteArrayLiteral("new-session"));
    QCOMPARE(datagram->peers.ports.first(), quint16(15550));
}

void UdpPeerStateTest::lifecycleRevisionInvalidatesQueuedTraffic()
{
    UdpPeerState state;
    const QHostAddress peer(QStringLiteral("203.0.113.30"));
    QCOMPARE(state.upsertPeer(peer, 14550).revision, quint64(1));
    QVERIFY(state.enqueueLatest(QByteArrayLiteral("before-reconnect")));

    QVERIFY(state.advanceRevision());
    const UdpPeerSnapshot after = state.snapshot();
    QCOMPARE(after.revision, quint64(2));
    QCOMPARE(after.hosts.first(), peer);
    QVERIFY(!state.takeNextCurrent());

    QVERIFY(!state.enqueueForRevision(QByteArrayLiteral("old"), 1));
    QVERIFY(state.enqueueForRevision(QByteArrayLiteral("after"), 2));
    QVERIFY(state.takeNextCurrent());
}

void UdpPeerStateTest::revisionExhaustionFailsClosedWithoutWrapping()
{
    UdpPeerState state(std::numeric_limits<quint64>::max());
    const UdpPeerUpdate update = state.upsertPeer(
            QHostAddress(QStringLiteral("192.0.2.50")), 14550);
    QVERIFY(!update.accepted);

    const UdpPeerSnapshot snapshot = state.snapshot();
    QCOMPARE(snapshot.revision, quint64(0));
    QVERIFY(snapshot.hosts.isEmpty());
    QVERIFY(snapshot.ports.isEmpty());
    QVERIFY(!state.enqueueForRevision(QByteArrayLiteral("blocked"), 0));
    QVERIFY(!state.advanceRevision());
}

QTEST_APPLESS_MAIN(UdpPeerStateTest)

#include "test_udplinkpeers.moc"
