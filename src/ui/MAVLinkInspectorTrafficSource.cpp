#include "MAVLinkInspectorTrafficSource.h"

#include <QThread>

MAVLinkInspectorTrafficSource::MAVLinkInspectorTrafficSource(QObject *parent)
    : QObject(parent)
    , m_status(tr("No MAVLink source selected."))
{
}

bool MAVLinkInspectorTrafficSource::bindLive(QObject *physicalLink,
                                             int linkId,
                                             quint64 currentEpoch,
                                             const QString &name)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_kind != SourceKind::Unbound || !physicalLink || linkId < 0) {
        return false;
    }

    m_kind = SourceKind::Live;
    m_physicalLink = physicalLink;
    m_liveLinkId = linkId;
    m_liveEpoch = currentEpoch;
    m_active = currentEpoch != 0;
    m_accepting = false;
    m_sourceName = name.trimmed();
    if (m_sourceName.isEmpty()) {
        m_sourceName = tr("Link %1").arg(linkId);
    }
    ++m_stateRevision;

    connect(physicalLink, &QObject::destroyed, this,
            &MAVLinkInspectorTrafficSource::liveObjectDestroyed);
    QPointer<MAVLinkInspectorTrafficSource> guard(this);
    const quint64 statusRevision = setStatus(
        currentEpoch == 0 ? liveDisconnectedStatus(false)
                          : liveWaitingStatus());
    if (currentEpoch != 0 && guard && statusRevision == m_stateRevision
        && liveSessionIsCurrent(physicalLink, linkId, currentEpoch)) {
        m_accepting = true;
        ++m_stateRevision;
    }
    return true;
}

bool MAVLinkInspectorTrafficSource::bindReplay(quint64 generation,
                                               const QString &name)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_kind != SourceKind::Unbound || generation == 0) {
        return false;
    }

    m_kind = SourceKind::Replay;
    m_replayGeneration = generation;
    m_active = true;
    m_accepting = false;
    m_sourceName = name.trimmed();
    if (m_sourceName.isEmpty()) {
        m_sourceName = tr("Telemetry replay");
    }
    ++m_stateRevision;
    QPointer<MAVLinkInspectorTrafficSource> guard(this);
    const quint64 statusRevision = setStatus(replayWaitingStatus());
    if (guard && statusRevision == m_stateRevision
        && replaySessionIsCurrent(generation)) {
        m_accepting = true;
        ++m_stateRevision;
    }
    return true;
}

QString MAVLinkInspectorTrafficSource::status() const
{
    return m_status;
}

void MAVLinkInspectorTrafficSource::observeLive(QObject *physicalLink,
                                                int linkId,
                                                quint64 epoch,
                                                mavlink_message_t message)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!liveSessionMatches(physicalLink, linkId, epoch)) {
        return;
    }

    QPointer<MAVLinkInspectorTrafficSource> guard(this);
    quint64 deliveryRevision = m_stateRevision;
    if (!m_receiving) {
        m_accepting = false;
        m_receiving = true;
        ++m_stateRevision;
        deliveryRevision = setStatus(liveReceivingStatus());

        if (!guard || deliveryRevision != m_stateRevision
            || !liveSessionIsCurrent(physicalLink, linkId, epoch)) {
            return;
        }
        m_accepting = true;
        ++m_stateRevision;
        deliveryRevision = m_stateRevision;
    }

    // A status observer may synchronously end/remove the source, roll it to a
    // newer epoch or close the inspector.  Never release the captured packet
    // after such a reentrant identity change.
    if (!guard || deliveryRevision != m_stateRevision
        || !liveSessionMatches(physicalLink, linkId, epoch)) {
        return;
    }
    emit messageReceived(message);
}

void MAVLinkInspectorTrafficSource::beginLiveSession(QObject *physicalLink,
                                                     int linkId,
                                                     quint64 epoch)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_kind != SourceKind::Live || m_terminal || !physicalLink
        || m_physicalLink.data() != physicalLink || m_liveLinkId != linkId
        || epoch == 0 || epoch <= m_liveEpoch) {
        return;
    }

    m_liveEpoch = epoch;
    m_active = true;
    m_accepting = false;
    m_receiving = false;
    ++m_stateRevision;
    const quint64 resetRevision = m_stateRevision;
    QPointer<MAVLinkInspectorTrafficSource> guard(this);
    emit sourceReset();

    // sourceReset is allowed to close the window or end the just-started
    // session.  In that case its status is authoritative.
    if (!guard || resetRevision != m_stateRevision
        || !liveSessionIsCurrent(physicalLink, linkId, epoch)) {
        return;
    }
    const quint64 statusRevision = setStatus(liveWaitingStatus());
    if (!guard || statusRevision != m_stateRevision
        || !liveSessionIsCurrent(physicalLink, linkId, epoch)) {
        return;
    }
    m_accepting = true;
    ++m_stateRevision;
}

void MAVLinkInspectorTrafficSource::endLiveSession(int linkId, quint64 epoch)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_kind != SourceKind::Live || m_terminal || !m_active
        || m_liveLinkId != linkId || m_liveEpoch != epoch) {
        return;
    }

    m_active = false;
    m_accepting = false;
    m_receiving = false;
    ++m_stateRevision;
    setStatus(liveDisconnectedStatus(true));
}

void MAVLinkInspectorTrafficSource::removeLiveLink(int linkId)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_kind != SourceKind::Live || m_terminal
        || m_liveLinkId != linkId) {
        return;
    }

    m_active = false;
    m_accepting = false;
    m_terminal = true;
    m_receiving = false;
    ++m_stateRevision;
    setStatus(tr("Live MAVLink source %1 is no longer available. Reopen "
                 "MAVLink Inspector to select another source.")
                  .arg(m_sourceName));
}

void MAVLinkInspectorTrafficSource::observeReplay(quint64 generation,
                                                  mavlink_message_t message)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!replaySessionMatches(generation)) {
        return;
    }

    QPointer<MAVLinkInspectorTrafficSource> guard(this);
    quint64 deliveryRevision = m_stateRevision;
    if (!m_receiving) {
        m_accepting = false;
        m_receiving = true;
        ++m_stateRevision;
        deliveryRevision = setStatus(replayReceivingStatus());

        if (!guard || deliveryRevision != m_stateRevision
            || !replaySessionIsCurrent(generation)) {
            return;
        }
        m_accepting = true;
        ++m_stateRevision;
        deliveryRevision = m_stateRevision;
    }

    if (!guard || deliveryRevision != m_stateRevision
        || !replaySessionMatches(generation)) {
        return;
    }
    emit messageReceived(message);
}

void MAVLinkInspectorTrafficSource::endReplay(quint64 generation)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!replaySessionIsCurrent(generation)) {
        return;
    }

    m_active = false;
    m_accepting = false;
    m_terminal = true;
    m_receiving = false;
    ++m_stateRevision;
    setStatus(tr("Replay MAVLink source %1 ended. Reopen MAVLink Inspector "
                 "to select another source.")
                  .arg(m_sourceName));
}

bool MAVLinkInspectorTrafficSource::liveSessionMatches(
    QObject *physicalLink, int linkId, quint64 epoch) const
{
    return m_kind == SourceKind::Live && m_active && !m_terminal
        && m_accepting && physicalLink && m_physicalLink
        && m_physicalLink.data() == physicalLink && m_liveLinkId == linkId
        && m_liveEpoch == epoch;
}

bool MAVLinkInspectorTrafficSource::liveSessionIsCurrent(
    QObject *physicalLink, int linkId, quint64 epoch) const
{
    return m_kind == SourceKind::Live && m_active && !m_terminal
        && physicalLink && m_physicalLink
        && m_physicalLink.data() == physicalLink && m_liveLinkId == linkId
        && m_liveEpoch == epoch;
}

bool MAVLinkInspectorTrafficSource::replaySessionMatches(
    quint64 generation) const
{
    return m_kind == SourceKind::Replay && m_active && !m_terminal
        && m_accepting && generation != 0
        && m_replayGeneration == generation;
}

bool MAVLinkInspectorTrafficSource::replaySessionIsCurrent(
    quint64 generation) const
{
    return m_kind == SourceKind::Replay && m_active && !m_terminal
        && generation != 0 && m_replayGeneration == generation;
}

quint64 MAVLinkInspectorTrafficSource::setStatus(const QString &status)
{
    if (m_status == status) {
        return m_stateRevision;
    }

    m_status = status;
    ++m_stateRevision;
    const quint64 emissionRevision = m_stateRevision;
    emit statusChanged(m_status);
    return emissionRevision;
}

QString MAVLinkInspectorTrafficSource::liveWaitingStatus() const
{
    return tr("Waiting for live MAVLink traffic from %1.").arg(m_sourceName);
}

QString MAVLinkInspectorTrafficSource::liveReceivingStatus() const
{
    return tr("Receiving live MAVLink traffic from %1.").arg(m_sourceName);
}

QString MAVLinkInspectorTrafficSource::liveDisconnectedStatus(
    bool reconnect) const
{
    return reconnect
        ? tr("Live MAVLink source %1 disconnected; waiting for the same link "
             "to reconnect.")
              .arg(m_sourceName)
        : tr("Live MAVLink source %1 is disconnected; waiting for this link "
             "to connect.")
              .arg(m_sourceName);
}

QString MAVLinkInspectorTrafficSource::replayWaitingStatus() const
{
    return tr("Waiting for replay MAVLink traffic from %1.")
        .arg(m_sourceName);
}

QString MAVLinkInspectorTrafficSource::replayReceivingStatus() const
{
    return tr("Receiving replay MAVLink traffic from %1.")
        .arg(m_sourceName);
}

void MAVLinkInspectorTrafficSource::liveObjectDestroyed()
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_kind != SourceKind::Live || m_terminal) {
        return;
    }

    m_active = false;
    m_accepting = false;
    m_terminal = true;
    m_receiving = false;
    ++m_stateRevision;
    setStatus(tr("Live MAVLink source %1 was destroyed. Reopen MAVLink "
                 "Inspector to select another source.")
                  .arg(m_sourceName));
}
