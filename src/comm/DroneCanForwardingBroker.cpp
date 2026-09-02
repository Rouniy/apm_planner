#include "DroneCanForwardingBroker.h"

#include <QDateTime>
#include <QThread>
#include <QTimer>

#include <limits>

namespace {
constexpr int kMavResultAccepted = 0;
constexpr int kMavResultInProgress = 5;

bool validMavlinkId(int value)
{
    return value > 0 && value <= 255;
}
}

bool DroneCanForwardingBroker::Endpoint::isValid() const
{
    return validMavlinkId(uasId)
        && validMavlinkId(gcsSystemId)
        && validMavlinkId(gcsComponentId)
        && validMavlinkId(autopilotComponentId)
        && linkId > 0;
}

bool DroneCanForwardingBroker::Endpoint::operator==(
    const Endpoint &other) const
{
    return uasId == other.uasId
        && gcsSystemId == other.gcsSystemId
        && gcsComponentId == other.gcsComponentId
        && autopilotComponentId == other.autopilotComponentId
        && linkId == other.linkId;
}

bool DroneCanForwardingBroker::LeaseToken::isValid() const
{
    return !owner.isNull() && sessionGeneration != 0
        && leaseGeneration != 0 && busIndex >= 0 && busIndex <= 1;
}

DroneCanForwardingBroker::DroneCanForwardingBroker(QObject *parent)
    : QObject(parent),
      m_keepaliveTimer(new QTimer(this))
{
    qRegisterMetaType<Endpoint>();
    qRegisterMetaType<LeaseToken>();
    m_keepaliveTimer->setObjectName(
        QStringLiteral("droneCanForwardingKeepaliveTimer"));
    m_keepaliveTimer->setInterval(KeepalivePeriodMs);
    m_keepaliveTimer->setTimerType(Qt::PreciseTimer);
    connect(m_keepaliveTimer, &QTimer::timeout, this, [this]() {
        tick(QDateTime::currentMSecsSinceEpoch());
    });
}

quint64 DroneCanForwardingBroker::bindSession(
    const Endpoint &endpoint, qint64 nowMs)
{
    Q_UNUSED(nowMs)
    assertBrokerThread();
    if (m_shutdown || !endpoint.isValid()) {
        m_lastError = tr("Invalid DroneCAN MAVLink endpoint.");
        return 0;
    }
    if (m_bound && m_endpoint == endpoint) {
        m_lastError.clear();
        return m_sessionGeneration;
    }

    if (m_bound) {
        if (m_activeBusIndex >= 0 || !m_leases.isEmpty()) {
            m_lastError = tr("Another DroneCAN endpoint is already active.");
            return 0;
        }
        invalidateSession(false, QString());
    }
    const quint64 generation = nextGeneration(m_sessionGeneration);
    if (generation == 0) {
        m_lastError = tr("DroneCAN session generation exhausted.");
        return 0;
    }

    m_endpoint = endpoint;
    m_sessionGeneration = generation;
    m_bound = true;
    m_confirmed = false;
    m_lastError.clear();
    emit stateChanged();
    return m_sessionGeneration;
}

void DroneCanForwardingBroker::transportLost(quint64 sessionGeneration)
{
    assertBrokerThread();
    if (!m_bound || sessionGeneration != m_sessionGeneration) {
        return;
    }
    invalidateSession(false, tr("DroneCAN MAVLink transport was lost."));
}

void DroneCanForwardingBroker::shutdown()
{
    assertBrokerThread();
    if (m_shutdown) {
        return;
    }
    if (m_bound) {
        invalidateSession(true, tr("DroneCAN broker is shutting down."));
    }
    m_shutdown = true;
}

DroneCanForwardingBroker::LeaseToken
DroneCanForwardingBroker::acquire(QObject *owner, int busIndex,
                                  qint64 nowMs)
{
    assertBrokerThread();
    if (m_shutdown || !m_bound || !owner || owner == this
        || owner->thread() != thread() || busIndex < 0 || busIndex > 1) {
        m_lastError = tr("DroneCAN forwarding endpoint is unavailable.");
        return {};
    }

    const auto existing = m_leases.constFind(owner);
    if (existing != m_leases.constEnd()) {
        if (m_activeBusIndex == busIndex) {
            return {owner, m_sessionGeneration,
                    existing->generation, busIndex};
        }
        m_lastError = tr("This consumer already owns another CAN bus.");
        return {};
    }
    if (m_activeBusIndex >= 0 && m_activeBusIndex != busIndex) {
        m_lastError = tr("CAN%1 is already shared by another DroneCAN tool.")
                          .arg(m_activeBusIndex + 1);
        return {};
    }

    const quint64 leaseGeneration = nextGeneration(m_leaseGeneration);
    if (leaseGeneration == 0) {
        m_lastError = tr("DroneCAN lease generation exhausted.");
        return {};
    }
    m_leaseGeneration = leaseGeneration;

    LeaseRecord record;
    record.generation = leaseGeneration;
    record.destroyedConnection = connect(
        owner, &QObject::destroyed, this,
        [this, owner, leaseGeneration]() {
            releaseOwner(owner, leaseGeneration);
        });
    m_leases.insert(owner, record);

    if (m_activeBusIndex < 0) {
        const quint64 sessionGeneration = m_sessionGeneration;
        m_activeBusIndex = busIndex;
        m_confirmed = false;
        m_startCommandSent = false;
        m_startedAtMs = 0;
        m_nextKeepaliveMs = 0;
        if (nowMs >= m_ackDrainUntilMs) {
            requestForwardingStart(nowMs);
        }
        // A direct transport slot can synchronously report failure and
        // invalidate this session while the command signal is being handled.
        const auto inserted = m_leases.constFind(owner);
        if (!m_bound || m_sessionGeneration != sessionGeneration
            || m_activeBusIndex != busIndex
            || inserted == m_leases.constEnd()
            || inserted->generation != leaseGeneration) {
            return {};
        }
        m_keepaliveTimer->start();
    }
    m_lastError.clear();
    emit stateChanged();
    return {owner, m_sessionGeneration, leaseGeneration, busIndex};
}

bool DroneCanForwardingBroker::owns(const LeaseToken &token) const
{
    assertBrokerThread();
    if (!token.isValid() || !m_bound
        || token.sessionGeneration != m_sessionGeneration
        || token.busIndex != m_activeBusIndex) {
        return false;
    }
    const auto found = m_leases.constFind(token.owner.data());
    return found != m_leases.constEnd()
        && found->generation == token.leaseGeneration;
}

bool DroneCanForwardingBroker::confirmed(const LeaseToken &token) const
{
    return owns(token) && m_confirmed;
}

bool DroneCanForwardingBroker::release(
    const LeaseToken &token, qint64 nowMs)
{
    assertBrokerThread();
    if (!owns(token)) {
        return false;
    }
    auto found = m_leases.find(token.owner.data());
    disconnect(found->destroyedConnection);
    m_leases.erase(found);
    if (m_leases.isEmpty()) {
        stopForwarding(true, nowMs);
    }
    emit stateChanged();
    return true;
}

bool DroneCanForwardingBroker::transmitFrame(
    const LeaseToken &token, quint32 canId, const QByteArray &data,
    bool canFd)
{
    assertBrokerThread();
    if (!owns(token) || !m_confirmed
        || !validDroneCanId(canId)
        || !validFramePayload(data.size(), canFd)) {
        m_lastError = tr("Invalid or inactive DroneCAN frame transport.");
        return false;
    }
    m_lastError.clear();
    emit frameTransmitRequested(
        m_sessionGeneration, m_endpoint.autopilotComponentId,
        m_activeBusIndex, canId, data, canFd);
    return m_bound && owns(token);
}

void DroneCanForwardingBroker::tick(qint64 nowMs)
{
    assertBrokerThread();
    if (!m_bound || m_activeBusIndex < 0 || m_leases.isEmpty()) {
        return;
    }
    if (!m_startCommandSent) {
        if (nowMs >= m_ackDrainUntilMs) {
            requestForwardingStart(nowMs);
        }
        // Start emission can synchronously invalidate the session.  In either
        // case, timeout/keepalive accounting starts on the next tick.
        return;
    }
    if (!m_confirmed && nowMs - m_startedAtMs >= StartupTimeoutMs) {
        invalidateSession(true,
                          tr("Timed out starting DroneCAN forwarding."),
                          nowMs);
        return;
    }
    if (nowMs < m_nextKeepaliveMs) {
        return;
    }
    m_nextKeepaliveMs = nowMs + KeepalivePeriodMs;
    emit forwardingCommandRequested(
        m_sessionGeneration, m_endpoint.autopilotComponentId,
        m_activeBusIndex + 1);
}

void DroneCanForwardingBroker::handleAck(
    quint64 sessionGeneration, int uasId, int sourceComponent,
    int command, int result, int targetSystem, int targetComponent)
{
    assertBrokerThread();
    if (!ackEnvelopeMatches(sessionGeneration, uasId, sourceComponent,
                            command, targetSystem, targetComponent)) {
        return;
    }
    if (result == kMavResultInProgress) {
        return;
    }
    if (result == kMavResultAccepted) {
        if (!m_confirmed) {
            m_confirmed = true;
            emit stateChanged();
        }
        return;
    }
    invalidateSession(
        true, tr("DroneCAN forwarding was rejected (MAV_RESULT %1).")
                  .arg(result));
}

void DroneCanForwardingBroker::handleFrame(
    quint64 sessionGeneration, int uasId, int sourceComponent,
    int targetSystem, int targetComponent, int busIndex,
    quint32 canId, const QByteArray &data, bool canFd, qint64 nowMs)
{
    assertBrokerThread();
    if (!frameEnvelopeMatches(sessionGeneration, uasId, sourceComponent,
                              targetSystem, targetComponent, busIndex,
                              canId, data.size(), canFd)) {
        return;
    }
    emit frameAccepted(sessionGeneration, busIndex, canId,
                       data, canFd, nowMs);
}

void DroneCanForwardingBroker::assertBrokerThread() const
{
    Q_ASSERT_X(QThread::currentThread() == thread(),
               "DroneCanForwardingBroker",
               "DroneCAN leases must be used on the broker thread");
}

bool DroneCanForwardingBroker::ackEnvelopeMatches(
    quint64 sessionGeneration, int uasId, int sourceComponent,
    int command, int targetSystem, int targetComponent) const
{
    return m_bound && m_activeBusIndex >= 0 && !m_leases.isEmpty()
        && m_startCommandSent
        && sessionGeneration == m_sessionGeneration
        && uasId == m_endpoint.uasId
        && sourceComponent == m_endpoint.autopilotComponentId
        && command == CanForwardCommand
        && (targetSystem == 0 || targetSystem == m_endpoint.gcsSystemId)
        && (targetComponent == 0
            || targetComponent == m_endpoint.gcsComponentId);
}

bool DroneCanForwardingBroker::frameEnvelopeMatches(
    quint64 sessionGeneration, int uasId, int sourceComponent,
    int targetSystem, int targetComponent, int busIndex,
    quint32 canId, int payloadSize, bool canFd) const
{
    return m_bound && m_activeBusIndex >= 0 && !m_leases.isEmpty()
        && m_startCommandSent
        && sessionGeneration == m_sessionGeneration
        && uasId == m_endpoint.uasId
        && sourceComponent == m_endpoint.autopilotComponentId
        && (targetSystem == 0 || targetSystem == m_endpoint.gcsSystemId)
        && (targetComponent == 0
            || targetComponent == m_endpoint.gcsComponentId)
        && busIndex == m_activeBusIndex
        && validDroneCanId(canId)
        && validFramePayload(payloadSize, canFd);
}

bool DroneCanForwardingBroker::validDroneCanId(quint32 canId)
{
    constexpr quint32 extendedFrameFlag = 0x80000000U;
    constexpr quint32 unsupportedFrameFlags = 0x60000000U;
    return (canId & extendedFrameFlag) != 0
        && (canId & unsupportedFrameFlags) == 0;
}

bool DroneCanForwardingBroker::validFramePayload(
    int payloadSize, bool canFd)
{
    // Every DroneCAN transport frame contains at least its tail byte.
    return payloadSize >= 1
        && ((!canFd && payloadSize <= 8)
            || (canFd
                && (payloadSize <= 8 || payloadSize == 12
                    || payloadSize == 16 || payloadSize == 20
                    || payloadSize == 24 || payloadSize == 32
                    || payloadSize == 48 || payloadSize == 64)));
}

void DroneCanForwardingBroker::releaseOwner(
    QObject *owner, quint64 leaseGeneration)
{
    assertBrokerThread();
    const auto found = m_leases.find(owner);
    if (found == m_leases.end()
        || found->generation != leaseGeneration) {
        return;
    }
    m_leases.erase(found);
    if (m_leases.isEmpty()) {
        stopForwarding(true, QDateTime::currentMSecsSinceEpoch());
    }
    emit stateChanged();
}

void DroneCanForwardingBroker::clearLeases()
{
    for (auto found = m_leases.begin(); found != m_leases.end(); ++found) {
        disconnect(found->destroyedConnection);
    }
    m_leases.clear();
}

void DroneCanForwardingBroker::requestForwardingStart(qint64 nowMs)
{
    if (!m_bound || m_activeBusIndex < 0 || m_leases.isEmpty()
        || m_startCommandSent) {
        return;
    }
    m_startCommandSent = true;
    m_confirmed = false;
    m_startedAtMs = nowMs;
    m_nextKeepaliveMs = nowMs + KeepalivePeriodMs;
    emit forwardingCommandRequested(
        m_sessionGeneration, m_endpoint.autopilotComponentId,
        m_activeBusIndex + 1);
}

void DroneCanForwardingBroker::stopForwarding(
    bool requestStop, qint64 nowMs)
{
    m_keepaliveTimer->stop();
    if (m_activeBusIndex < 0) {
        return;
    }

    const bool sendStop = requestStop && m_bound && m_startCommandSent;
    const quint64 sessionGeneration = m_sessionGeneration;
    const int targetComponent = m_endpoint.autopilotComponentId;

    // Clear the active command state before emitting.  A direct transport
    // slot may synchronously report loss, while an ACK for the stop command
    // must never confirm a later lease.
    m_activeBusIndex = -1;
    m_confirmed = false;
    m_startCommandSent = false;
    m_startedAtMs = 0;
    m_nextKeepaliveMs = 0;

    if (sendStop) {
        const qint64 stoppedAtMs = nowMs >= 0
            ? nowMs : QDateTime::currentMSecsSinceEpoch();
        m_ackDrainUntilMs = qMax(
            m_ackDrainUntilMs, stoppedAtMs + AckDrainPeriodMs);
        emit forwardingCommandRequested(
            sessionGeneration, targetComponent, 0);
    }
}

void DroneCanForwardingBroker::invalidateSession(
    bool requestStop, const QString &reason, qint64 nowMs)
{
    const quint64 invalidatedGeneration = m_sessionGeneration;
    stopForwarding(requestStop, nowMs);
    clearLeases();
    m_bound = false;
    m_confirmed = false;
    m_endpoint = {};
    m_lastError = reason;
    if (!reason.isEmpty()) {
        emit sessionFailed(invalidatedGeneration, reason);
    }
    emit sessionInvalidated(invalidatedGeneration);
    emit stateChanged();
}

quint64 DroneCanForwardingBroker::nextGeneration(quint64 current) const
{
    return current == std::numeric_limits<quint64>::max()
        ? 0 : current + 1;
}
