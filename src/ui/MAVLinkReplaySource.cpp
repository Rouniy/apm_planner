#include "MAVLinkReplaySource.h"

#include <QThread>

MAVLinkReplaySource::MAVLinkReplaySource(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<mavlink_message_t>("mavlink_message_t");
    qRegisterMetaType<MAVLinkReplayLease>("MAVLinkReplayLease");
}

MAVLinkReplayLease MAVLinkReplaySource::beginSource(
    const QString &displayName)
{
    Q_ASSERT(QThread::currentThread() == thread());

    // Replacing an active source without ending it would make lifetime
    // ordering ambiguous. The player always invalidates the old lease first.
    if (m_activeLease.isValid()) {
        return {};
    }

    ++m_lastGeneration;
    if (m_lastGeneration == 0) {
        ++m_lastGeneration;
    }

    m_activeLease.generation = m_lastGeneration;
    m_activeLease.displayName = displayName;
    return m_activeLease;
}

MAVLinkReplayLease MAVLinkReplaySource::activeLease() const
{
    Q_ASSERT(QThread::currentThread() == thread());
    return m_activeLease;
}

bool MAVLinkReplaySource::isActive(quint64 generation) const
{
    Q_ASSERT(QThread::currentThread() == thread());
    return generation != 0 && m_activeLease.generation == generation;
}

bool MAVLinkReplaySource::publish(
    quint64 generation,
    const mavlink_message_t &message)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!isActive(generation)) {
        return false;
    }

    emit replayMessageObserved(generation, message);
    return true;
}

bool MAVLinkReplaySource::endSource(quint64 expectedGeneration)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!isActive(expectedGeneration)) {
        return false;
    }

    const quint64 endedGeneration = m_activeLease.generation;
    m_activeLease = {};
    emit replaySourceEnded(endedGeneration);
    return true;
}
