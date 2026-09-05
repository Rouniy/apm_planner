#include "UdpPeerState.h"

#include <QMutexLocker>

#include <limits>

UdpPeerState::UdpPeerState(quint64 initialRevision)
    : m_revision(initialRevision)
{
}

UdpPeerSnapshot UdpPeerState::snapshot() const
{
    QMutexLocker locker(&m_mutex);
    return snapshotLocked();
}

UdpPeerUpdate UdpPeerState::upsertPeer(const QHostAddress &host, quint16 port)
{
    QMutexLocker locker(&m_mutex);
    if (m_revisionExhausted || host.isNull() || port == 0) {
        return {};
    }

    const int index = m_hosts.indexOf(host);
    if (index >= 0 && m_ports.at(index) == port) {
        return {true, false, m_revision};
    }
    if (!advanceRevisionLocked()) {
        return {};
    }

    if (index >= 0) {
        m_ports.replace(index, port);
    } else {
        m_hosts.append(host);
        m_ports.append(port);
    }
    return {true, true, m_revision};
}

UdpPeerUpdate UdpPeerState::removePeer(const QHostAddress &host)
{
    QMutexLocker locker(&m_mutex);
    if (m_revisionExhausted || host.isNull()) {
        return {};
    }

    const int index = m_hosts.indexOf(host);
    if (index < 0) {
        return {true, false, m_revision};
    }
    if (!advanceRevisionLocked()) {
        return {};
    }

    // upsertPeer keeps host identity unique, so one paired removal is enough.
    m_hosts.removeAt(index);
    m_ports.removeAt(index);
    return {true, true, m_revision};
}

bool UdpPeerState::advanceRevision()
{
    QMutexLocker locker(&m_mutex);
    return advanceRevisionLocked();
}

bool UdpPeerState::enqueueLatest(const QByteArray &bytes)
{
    QMutexLocker locker(&m_mutex);
    return enqueueLocked(bytes, m_revision);
}

bool UdpPeerState::enqueueForRevision(const QByteArray &bytes,
                                      quint64 expectedRevision)
{
    QMutexLocker locker(&m_mutex);
    return enqueueLocked(bytes, expectedRevision);
}

std::optional<UdpPeerDatagram> UdpPeerState::takeNextCurrent()
{
    QMutexLocker locker(&m_mutex);
    while (!m_outQueue.isEmpty()) {
        UdpPeerDatagram datagram = m_outQueue.dequeue();
        if (!m_revisionExhausted
                && datagram.peers.revision == m_revision
                && !datagram.peers.hosts.isEmpty()
                && datagram.peers.hosts.size() == datagram.peers.ports.size()) {
            return datagram;
        }
    }
    return std::nullopt;
}

bool UdpPeerState::hasQueuedDatagrams() const
{
    QMutexLocker locker(&m_mutex);
    return !m_outQueue.isEmpty();
}

int UdpPeerState::queuedDatagramCount() const
{
    QMutexLocker locker(&m_mutex);
    return m_outQueue.size();
}

UdpPeerSnapshot UdpPeerState::snapshotLocked() const
{
    if (m_revisionExhausted) {
        return {};
    }
    return {m_revision, m_hosts, m_ports};
}

bool UdpPeerState::advanceRevisionLocked()
{
    if (m_revisionExhausted) {
        return false;
    }
    if (m_revision == std::numeric_limits<quint64>::max()) {
        // A wrapped identity could authorize bytes for a different peer set.
        // Exhaustion is practically unreachable, but it must fail closed.
        m_revisionExhausted = true;
        m_hosts.clear();
        m_ports.clear();
        m_outQueue.clear();
        return false;
    }
    ++m_revision;
    return true;
}

bool UdpPeerState::enqueueLocked(const QByteArray &bytes,
                                 quint64 expectedRevision)
{
    if (m_revisionExhausted || bytes.isEmpty()
            || expectedRevision != m_revision || m_hosts.isEmpty()
            || m_hosts.size() != m_ports.size()) {
        return false;
    }

    m_outQueue.enqueue({bytes, snapshotLocked()});
    return true;
}
