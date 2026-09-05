#ifndef UDPPEERSTATE_H
#define UDPPEERSTATE_H

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QMutex>
#include <QQueue>

#include <optional>

struct UdpPeerSnapshot
{
    quint64 revision = 0;
    QList<QHostAddress> hosts;
    QList<quint16> ports;
};

struct UdpPeerUpdate
{
    bool accepted = false;
    bool changed = false;
    quint64 revision = 0;
};

struct UdpPeerDatagram
{
    QByteArray bytes;
    UdpPeerSnapshot peers;
};

/**
 * Thread-safe UDP peer identity and revision-bound outbound queue.
 *
 * A revision identifies the complete ordered peer/port set. Queued bytes keep
 * the snapshot from enqueue time and are discarded if that identity changes.
 */
class UdpPeerState final
{
public:
    explicit UdpPeerState(quint64 initialRevision = 0);

    UdpPeerSnapshot snapshot() const;

    UdpPeerUpdate upsertPeer(const QHostAddress &host, quint16 port);
    UdpPeerUpdate removePeer(const QHostAddress &host);

    /** Invalidate queued traffic for a transport lifecycle transition. */
    bool advanceRevision();

    /** Legacy enqueue using the peer identity current at the call boundary. */
    bool enqueueLatest(const QByteArray &bytes);
    /** Strict enqueue that fails rather than targeting a newer peer identity. */
    bool enqueueForRevision(const QByteArray &bytes,
                            quint64 expectedRevision);

    /** Drop stale envelopes and return the next datagram for the current peer. */
    std::optional<UdpPeerDatagram> takeNextCurrent();
    bool hasQueuedDatagrams() const;
    int queuedDatagramCount() const;

private:
    UdpPeerSnapshot snapshotLocked() const;
    bool advanceRevisionLocked();
    bool enqueueLocked(const QByteArray &bytes, quint64 expectedRevision);

    mutable QMutex m_mutex;
    quint64 m_revision = 0;
    bool m_revisionExhausted = false;
    QList<QHostAddress> m_hosts;
    QList<quint16> m_ports;
    QQueue<UdpPeerDatagram> m_outQueue;
};

#endif // UDPPEERSTATE_H
