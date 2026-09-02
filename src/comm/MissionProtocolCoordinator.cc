#include "MissionProtocolCoordinator.h"

#include <QThread>

#include <limits>

MissionProtocolCoordinator::MissionProtocolCoordinator(QObject *parent)
    : QObject(parent)
{
}

MissionProtocolCoordinator::LeaseToken
MissionProtocolCoordinator::tryAcquire(QObject *leaseOwner,
                                       MissionType requestedMissionType)
{
    assertCoordinatorThread();

    if (!leaseOwner || leaseOwner == this
        || leaseOwner->thread() != thread()
        || !isSupportedMissionType(requestedMissionType)) {
        return {};
    }

    if (m_owner) {
        if (m_owner == leaseOwner && m_missionType == requestedMissionType) {
            return {m_owner, m_missionType, m_generation};
        }
        return {};
    }

    if (m_generation == std::numeric_limits<quint64>::max()) {
        // Zero is reserved for an invalid token. Reaching this limit would
        // require exhausting the entire 64-bit generation space, so refuse a
        // new lease rather than making an old token valid again.
        return {};
    }

    ++m_generation;
    m_owner = leaseOwner;
    m_missionType = requestedMissionType;
    const quint64 acquiredGeneration = m_generation;
    m_ownerDestroyedConnection = connect(
        leaseOwner, &QObject::destroyed, this,
        [this, acquiredGeneration]() {
            assertCoordinatorThread();
            if (m_generation == acquiredGeneration) {
                clearLease();
            }
        });
    return {m_owner, m_missionType, m_generation};
}

bool MissionProtocolCoordinator::owns(const LeaseToken &token) const
{
    assertCoordinatorThread();
    return token.isValid()
        && !m_owner.isNull()
        && token.owner == m_owner
        && token.missionType == m_missionType
        && token.generation == m_generation;
}

bool MissionProtocolCoordinator::release(const LeaseToken &token)
{
    assertCoordinatorThread();
    if (!owns(token)) {
        return false;
    }
    clearLease();
    return true;
}

QObject *MissionProtocolCoordinator::owner() const
{
    assertCoordinatorThread();
    return m_owner.data();
}

MissionProtocolCoordinator::MissionType
MissionProtocolCoordinator::missionType() const
{
    assertCoordinatorThread();
    return m_missionType;
}

quint64 MissionProtocolCoordinator::generation() const
{
    assertCoordinatorThread();
    return m_generation;
}

bool MissionProtocolCoordinator::isSupportedMissionType(MissionType type)
{
    switch (type) {
    case MissionType::Mission:
    case MissionType::Fence:
    case MissionType::Rally:
        return true;
    }
    return false;
}

void MissionProtocolCoordinator::assertCoordinatorThread() const
{
    Q_ASSERT_X(QThread::currentThread() == thread(),
               "MissionProtocolCoordinator",
               "Mission protocol leases must be used on the coordinator thread");
}

void MissionProtocolCoordinator::clearLease()
{
    if (m_ownerDestroyedConnection) {
        disconnect(m_ownerDestroyedConnection);
        m_ownerDestroyedConnection = {};
    }
    m_owner.clear();
}
