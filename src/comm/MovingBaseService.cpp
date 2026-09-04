#include "MovingBaseService.h"

#include "VehicleTargetManager.h"

#include <QThread>
#include <QtMath>

#include <cmath>
#include <limits>
#include <utility>

MovingBaseService::MovingBaseService(
    VehicleTargetManager *targetManager,
    MovingBasePositionStore *positionStore,
    QObject *parent)
    : MovingBaseService(targetManager, positionStore, TimingSeams{}, parent)
{
}

MovingBaseService::MovingBaseService(
    VehicleTargetManager *targetManager,
    MovingBasePositionStore *positionStore,
    TimingSeams timingSeams,
    QObject *parent)
    : QObject(parent)
    , m_targetManager(targetManager)
    , m_positionStore(positionStore)
    , m_timingSeams(std::move(timingSeams))
{
    Q_ASSERT(targetManager);
    Q_ASSERT(positionStore);
    qRegisterMetaType<MovingBaseService::SessionToken>();
    qRegisterMetaType<MovingBaseService::State>();
    qRegisterMetaType<MovingBaseService::RequestResult>();

    m_monotonicClock.start();
    m_wakeupTimer.setSingleShot(true);
    connect(&m_wakeupTimer, &QTimer::timeout,
            this, &MovingBaseService::processTimersForTesting);
    if (targetManager) {
        connect(targetManager,
                &VehicleTargetManager::targetGenerationChanged,
                this, &MovingBaseService::handleTargetGenerationChanged);
        connect(targetManager, &QObject::destroyed, this, [this]() {
            if (hasActiveSession()) {
                finishSession(
                    RequestResult::InvalidTarget,
                    tr("The vehicle target service closed; Moving Base "
                       "stopped."));
            }
        });
    }
    if (positionStore) {
        connect(positionStore, &QObject::destroyed, this, [this]() {
            if (hasActiveSession()) {
                finishSession(
                    RequestResult::StorageUnavailable,
                    tr("The Moving Base position store closed; the session "
                       "stopped."));
            }
        });
    }
}

MovingBaseService::~MovingBaseService()
{
    disarmWakeup();
    if (m_ownerDestroyedConnection) {
        disconnect(m_ownerDestroyedConnection);
    }
    if (hasActiveSession() && m_positionStore) {
        m_positionStore->clear(m_session.target);
    }
}

int MovingBaseService::staleTimeoutMs() const noexcept
{
    if (!isSupportedRate(m_rateHz)) {
        return MinimumStaleTimeoutMs;
    }
    return qMax(MinimumStaleTimeoutMs,
                qRound(3000.0 / m_rateHz));
}

QList<double> MovingBaseService::supportedRates()
{
    return {0.25, 0.5, 1.0, 2.0};
}

bool MovingBaseService::isSupportedRate(double rateHz) noexcept
{
    return std::isfinite(rateHz)
        && (rateHz == 0.25 || rateHz == 0.5
            || rateHz == 1.0 || rateHz == 2.0);
}

QString MovingBaseService::stateDescription(State state)
{
    switch (state) {
    case State::Idle:
        return tr("Idle");
    case State::Active:
        return tr("Active");
    }
    return tr("Unknown");
}

QString MovingBaseService::resultDescription(RequestResult result)
{
    switch (result) {
    case RequestResult::Started:
        return tr("Moving Base session started.");
    case RequestResult::Published:
        return tr("Moving Base position published.");
    case RequestResult::Queued:
        return tr("Newest Moving Base position queued.");
    case RequestResult::Cleared:
        return tr("Moving Base position cleared.");
    case RequestResult::Stopped:
        return tr("Moving Base session stopped.");
    case RequestResult::Busy:
        return tr("Another window owns the Moving Base session.");
    case RequestResult::InvalidOwner:
        return tr("A live owner on the service thread is required.");
    case RequestResult::InvalidSession:
        return tr("The Moving Base session token is no longer current.");
    case RequestResult::InvalidTarget:
        return tr("No valid exact vehicle target is selected.");
    case RequestResult::StaleTarget:
        return tr("The exact vehicle target changed.");
    case RequestResult::TargetUnsettled:
        return tr("The vehicle selection is still changing.");
    case RequestResult::InvalidRate:
        return tr("Select a Moving Base rate of 0.25, 0.5, 1 or 2 Hz.");
    case RequestResult::InvalidFix:
        return tr("The parsed GGA position fix is invalid.");
    case RequestResult::StorageUnavailable:
        return tr("The Moving Base position store is unavailable.");
    }
    return tr("Unknown Moving Base result.");
}

MovingBaseService::RequestResult MovingBaseService::start(
    QObject *owner, const VehicleTargetLease &target,
    double rateHz, SessionToken *sessionOut)
{
    if (sessionOut) {
        *sessionOut = SessionToken{};
    }
    if (!owner || owner == this || owner->thread() != thread()
        || QThread::currentThread() != thread()) {
        return RequestResult::InvalidOwner;
    }
    if (m_finishing || hasActiveSession()) {
        return RequestResult::Busy;
    }
    if (!isSupportedRate(rateHz)) {
        return RequestResult::InvalidRate;
    }
    const RequestResult targetResult = validateStartTarget(target);
    if (targetResult != RequestResult::Started) {
        return targetResult;
    }
    if (!m_positionStore) {
        return RequestResult::StorageUnavailable;
    }

    const quint64 generation = nextSessionGeneration();
    if (generation == 0) {
        return RequestResult::InvalidSession;
    }

    m_session.owner = owner;
    m_session.generation = generation;
    m_session.target = target;
    m_rateHz = rateHz;
    resetFixState();
    m_state = State::Active;
    m_statusText = tr(
        "Moving Base is active for the exact vehicle; waiting for a valid "
        "GGA fix.");
    const quint64 transition = ++m_transitionGeneration;
    m_ownerDestroyedConnection = connect(
        owner, &QObject::destroyed, this,
        [this, generation]() { handleOwnerDestroyed(generation); });

    // A new input session must never expose an older producer's marker for the
    // same target generation while it waits for its first valid fix.
    QPointer<MovingBaseService> guard(this);
    m_positionStore->clear(target);
    if (!guard) {
        return RequestResult::InvalidSession;
    }
    if (!hasActiveSession() || m_session.generation != generation) {
        return m_lastFinishedSessionGeneration == generation
            ? m_lastFinishedResult : RequestResult::InvalidSession;
    }

    publishStateAndStatus(transition);
    if (!guard) {
        return RequestResult::InvalidSession;
    }
    if (!hasActiveSession() || m_session.generation != generation) {
        return m_lastFinishedSessionGeneration == generation
            ? m_lastFinishedResult : RequestResult::InvalidSession;
    }
    if (sessionOut) {
        *sessionOut = m_session;
    }
    return RequestResult::Started;
}

MovingBaseService::RequestResult MovingBaseService::submitFix(
    const SessionToken &session, const NmeaGgaFix &fix)
{
    if (!sessionMatches(session)) {
        return RequestResult::InvalidSession;
    }
    if (!targetIsCurrent(m_session.target)) {
        finishSession(
            RequestResult::StaleTarget,
            tr("The selected vehicle changed; Moving Base stopped."));
        return RequestResult::StaleTarget;
    }
    if (!isValidFix(fix)) {
        return RequestResult::InvalidFix;
    }

    const qint64 observedMs = nowMs();
    m_pendingFix = fix;
    m_pendingObservedMs = observedMs;
    m_hasPendingFix = true;
    m_lastValidFixMs = observedMs;

    if (m_lastPublishedMs < 0
        || observedMs - m_lastPublishedMs >= rateIntervalMs()) {
        return publishPendingFix();
    }

    m_statusText = tr(
        "Moving Base is active; the newest valid GGA fix is queued for the "
        "%1 Hz rate limit.").arg(m_rateHz, 0, 'g', 2);
    const quint64 transition = ++m_transitionGeneration;
    scheduleNextWakeup();
    QPointer<MovingBaseService> guard(this);
    publishStateAndStatus(transition);
    if (!guard) {
        return RequestResult::InvalidSession;
    }
    return sessionMatches(session)
        ? RequestResult::Queued
        : (m_lastFinishedSessionGeneration == session.generation
               ? m_lastFinishedResult : RequestResult::InvalidSession);
}

MovingBaseService::RequestResult MovingBaseService::reportNoFix(
    const SessionToken &session, const QString &description)
{
    if (!sessionMatches(session)) {
        return RequestResult::InvalidSession;
    }
    if (!targetIsCurrent(m_session.target)) {
        finishSession(
            RequestResult::StaleTarget,
            tr("The selected vehicle changed; Moving Base stopped."));
        return RequestResult::StaleTarget;
    }
    const QString reason = description.trimmed().isEmpty()
        ? tr("The GGA source reports no position fix; the Moving Base "
             "position was cleared.")
        : tr("%1 The Moving Base position was cleared.")
              .arg(description.trimmed());
    return clearPosition(reason);
}

MovingBaseService::RequestResult MovingBaseService::stop(
    const SessionToken &session)
{
    if (!sessionMatches(session)) {
        return RequestResult::InvalidSession;
    }
    finishSession(RequestResult::Stopped,
                  tr("Moving Base stopped and its position was cleared."));
    return RequestResult::Stopped;
}

void MovingBaseService::forgetLink(int linkId)
{
    if (linkId < 0 || !hasActiveSession()
        || m_session.target.endpoint.linkId != linkId) {
        return;
    }
    finishSession(
        RequestResult::Stopped,
        tr("The physical link closed; Moving Base stopped and its position "
           "was cleared."));
}

void MovingBaseService::processTimersForTesting()
{
    disarmWakeup();
    if (!hasActiveSession()) {
        return;
    }
    if (!targetIsCurrent(m_session.target)) {
        finishSession(
            RequestResult::StaleTarget,
            tr("The selected vehicle changed; Moving Base stopped."));
        return;
    }

    const qint64 now = nowMs();
    if (m_lastValidFixMs >= 0
        && now - m_lastValidFixMs >= staleTimeoutMs()) {
        clearPosition(
            tr("No fresh GGA fix arrived for %1 seconds; the Moving Base "
               "position was cleared while the input session remains active.")
                .arg(staleTimeoutMs() / 1000.0, 0, 'g', 3));
        return;
    }
    if (m_hasPendingFix && m_lastPublishedMs >= 0
        && now - m_lastPublishedMs >= rateIntervalMs()) {
        publishPendingFix();
        return;
    }
    scheduleNextWakeup();
}

bool MovingBaseService::targetIsCurrent(
    const VehicleTargetLease &target) const
{
    return m_targetManager && target.isValid()
        && m_targetManager->isCurrentTarget(
            target.endpoint.linkId, target.endpoint.systemId,
            target.endpoint.componentId, target.generation);
}

bool MovingBaseService::sessionMatches(
    const SessionToken &session) const
{
    return session.isValid() && hasActiveSession()
        && session.generation == m_session.generation
        && session.owner == m_session.owner
        && session.target.generation == m_session.target.generation
        && session.target.endpoint.sameIdentity(m_session.target.endpoint);
}

MovingBaseService::RequestResult MovingBaseService::validateStartTarget(
    const VehicleTargetLease &target) const
{
    if (!m_targetManager || !target.isValid()) {
        return RequestResult::InvalidTarget;
    }
    if (!targetIsCurrent(target)) {
        return RequestResult::StaleTarget;
    }
    if (!m_targetManager->isTargetGenerationSettled()) {
        return RequestResult::TargetUnsettled;
    }
    return RequestResult::Started;
}

bool MovingBaseService::isValidFix(const NmeaGgaFix &fix) noexcept
{
    return std::isfinite(fix.latitude)
        && std::isfinite(fix.longitude)
        && std::isfinite(fix.altitudeM)
        && std::isfinite(fix.hdop)
        && (!fix.hasGeoidSeparation
            || std::isfinite(fix.geoidSeparationM))
        && fix.latitude >= -90.0 && fix.latitude <= 90.0
        && fix.longitude >= -180.0 && fix.longitude <= 180.0
        && fix.satellites >= 0 && fix.hdop >= 0.0
        && fix.fixQuality > 0;
}

MovingBasePositionFix MovingBaseService::positionFix(
    const NmeaGgaFix &fix, qint64 observedMs) const
{
    MovingBasePositionFix position;
    position.latitudeDegrees = fix.latitude;
    position.longitudeDegrees = fix.longitude;
    // NMEA GGA field 9 is altitude above mean sea level. Geoid separation is
    // not added here because that would convert the value toward ellipsoid
    // height and violate MovingBasePositionStore's AMSL contract.
    position.altitudeAmslMetres = fix.altitudeM;
    position.satellites = fix.satellites;
    position.hdop = fix.hdop;
    position.observedMonotonicMs = observedMs;
    return position;
}

MovingBaseService::RequestResult MovingBaseService::publishPendingFix()
{
    if (!hasActiveSession() || !m_hasPendingFix) {
        return RequestResult::InvalidSession;
    }
    if (!m_positionStore) {
        finishSession(
            RequestResult::StorageUnavailable,
            resultDescription(RequestResult::StorageUnavailable));
        return RequestResult::StorageUnavailable;
    }
    if (!targetIsCurrent(m_session.target)) {
        finishSession(
            RequestResult::StaleTarget,
            tr("The selected vehicle changed; Moving Base stopped."));
        return RequestResult::StaleTarget;
    }

    const SessionToken publishingSession = m_session;
    const MovingBasePositionFix publishedFix = positionFix(
        m_pendingFix, m_pendingObservedMs);
    m_hasPendingFix = false;
    m_pendingObservedMs = -1;
    m_lastPublishedMs = nowMs();

    QPointer<MovingBaseService> guard(this);
    const bool stored = m_positionStore->update(
        publishingSession.target, publishedFix);
    if (!guard) {
        return RequestResult::InvalidSession;
    }
    if (!stored) {
        const RequestResult failure = targetIsCurrent(publishingSession.target)
            ? RequestResult::StorageUnavailable
            : RequestResult::StaleTarget;
        finishSession(failure, resultDescription(failure));
        return failure;
    }
    if (!sessionMatches(publishingSession)) {
        return m_lastFinishedSessionGeneration == publishingSession.generation
            ? m_lastFinishedResult : RequestResult::InvalidSession;
    }

    const MovingBasePositionSnapshot snapshot =
        m_positionStore->snapshot(publishingSession.target);
    m_statusText = tr(
        "Moving Base position published (%1 satellites, HDOP %2).")
        .arg(publishedFix.satellites)
        .arg(publishedFix.hdop, 0, 'g', 3);
    const quint64 transition = ++m_transitionGeneration;
    scheduleNextWakeup();
    emit fixPublished(publishingSession, snapshot);
    if (!guard) {
        return RequestResult::InvalidSession;
    }
    if (transition != m_transitionGeneration
        || !sessionMatches(publishingSession)) {
        return m_lastFinishedSessionGeneration == publishingSession.generation
            ? m_lastFinishedResult : RequestResult::InvalidSession;
    }
    publishStateAndStatus(transition);
    if (!guard) {
        return RequestResult::InvalidSession;
    }
    return sessionMatches(publishingSession)
        ? RequestResult::Published
        : (m_lastFinishedSessionGeneration == publishingSession.generation
               ? m_lastFinishedResult : RequestResult::InvalidSession);
}

MovingBaseService::RequestResult MovingBaseService::clearPosition(
    const QString &reason)
{
    if (!hasActiveSession()) {
        return RequestResult::InvalidSession;
    }
    if (!m_positionStore) {
        finishSession(
            RequestResult::StorageUnavailable,
            resultDescription(RequestResult::StorageUnavailable));
        return RequestResult::StorageUnavailable;
    }

    const SessionToken clearingSession = m_session;
    resetFixState();
    QPointer<MovingBaseService> guard(this);
    m_positionStore->clear(clearingSession.target);
    if (!guard) {
        return RequestResult::InvalidSession;
    }
    if (!sessionMatches(clearingSession)) {
        return m_lastFinishedSessionGeneration == clearingSession.generation
            ? m_lastFinishedResult : RequestResult::InvalidSession;
    }

    m_statusText = reason;
    const quint64 transition = ++m_transitionGeneration;
    emit positionCleared(clearingSession, reason);
    if (!guard) {
        return RequestResult::InvalidSession;
    }
    if (transition != m_transitionGeneration
        || !sessionMatches(clearingSession)) {
        return m_lastFinishedSessionGeneration == clearingSession.generation
            ? m_lastFinishedResult : RequestResult::InvalidSession;
    }
    publishStateAndStatus(transition);
    if (!guard) {
        return RequestResult::InvalidSession;
    }
    return sessionMatches(clearingSession)
        ? RequestResult::Cleared
        : (m_lastFinishedSessionGeneration == clearingSession.generation
               ? m_lastFinishedResult : RequestResult::InvalidSession);
}

void MovingBaseService::handleTargetGenerationChanged(qulonglong generation)
{
    Q_UNUSED(generation)
    if (!m_finishing && hasActiveSession()
        && !targetIsCurrent(m_session.target)) {
        finishSession(
            RequestResult::StaleTarget,
            tr("The selected vehicle changed; Moving Base stopped and its "
               "current position was cleared."));
    }
}

void MovingBaseService::handleOwnerDestroyed(quint64 sessionGeneration)
{
    if (!m_finishing && hasActiveSession()
        && m_session.generation == sessionGeneration) {
        finishSession(
            RequestResult::Stopped,
            tr("The Moving Base owner closed; the session stopped and its "
               "position was cleared."));
    }
}

void MovingBaseService::finishSession(
    RequestResult reason, const QString &description)
{
    if (!hasActiveSession() || m_finishing) {
        return;
    }
    m_finishing = true;
    const SessionToken endedSession = m_session;
    if (m_ownerDestroyedConnection) {
        disconnect(m_ownerDestroyedConnection);
        m_ownerDestroyedConnection = {};
    }
    resetFixState();
    QPointer<MovingBaseService> guard(this);
    if (m_positionStore) {
        m_positionStore->clear(endedSession.target);
    }
    if (!guard) {
        return;
    }

    m_lastFinishedSessionGeneration = endedSession.generation;
    m_lastFinishedResult = reason;
    m_session = SessionToken{};
    m_state = State::Idle;
    m_rateHz = DefaultRateHz;
    m_statusText = description;
    const quint64 transition = ++m_transitionGeneration;
    m_finishing = false;

    emit sessionEnded(endedSession, reason, description);
    if (!guard) {
        return;
    }
    if (transition != m_transitionGeneration) {
        return;
    }
    // On a target-generation change the store rejects mutation through the
    // stale lease by design, but its currentSnapshot is already empty. Publish
    // the service-level clear transition in either case so UI consumers never
    // retain the old marker.
    emit positionCleared(endedSession, description);
    if (!guard) {
        return;
    }
    if (transition != m_transitionGeneration) {
        return;
    }
    publishStateAndStatus(transition);
}

void MovingBaseService::resetFixState()
{
    disarmWakeup();
    m_pendingFix = NmeaGgaFix{};
    m_hasPendingFix = false;
    m_pendingObservedMs = -1;
    m_lastValidFixMs = -1;
    m_lastPublishedMs = -1;
}

void MovingBaseService::scheduleNextWakeup()
{
    if (!hasActiveSession() || m_lastValidFixMs < 0) {
        disarmWakeup();
        return;
    }

    qint64 deadline = m_lastValidFixMs + staleTimeoutMs();
    if (m_hasPendingFix && m_lastPublishedMs >= 0) {
        deadline = qMin(deadline,
                        m_lastPublishedMs + rateIntervalMs());
    }
    const qint64 remaining = qMax<qint64>(1, deadline - nowMs());
    armWakeup(static_cast<int>(qMin<qint64>(
        remaining, std::numeric_limits<int>::max())));
}

void MovingBaseService::armWakeup(int delayMs)
{
    const int boundedDelay = qMax(1, delayMs);
    if (m_timingSeams.armOneShot) {
        m_wakeupTimer.stop();
        m_timingSeams.armOneShot(boundedDelay);
        return;
    }
    m_wakeupTimer.start(boundedDelay);
}

void MovingBaseService::disarmWakeup()
{
    m_wakeupTimer.stop();
    if (m_timingSeams.disarm) {
        m_timingSeams.disarm();
    }
}

qint64 MovingBaseService::nowMs() const
{
    if (m_timingSeams.monotonicMs) {
        return qMax<qint64>(0, m_timingSeams.monotonicMs());
    }
    return qMax<qint64>(0, m_monotonicClock.elapsed());
}

int MovingBaseService::rateIntervalMs() const noexcept
{
    return isSupportedRate(m_rateHz)
        ? qRound(1000.0 / m_rateHz) : 2000;
}

void MovingBaseService::publishStateAndStatus(quint64 transition)
{
    if (transition != m_transitionGeneration) {
        return;
    }
    const State state = m_state;
    const QString status = m_statusText;
    QPointer<MovingBaseService> guard(this);
    emit stateChanged(state);
    if (!guard) {
        return;
    }
    if (transition != m_transitionGeneration) {
        return;
    }
    emit statusChanged(status);
}

quint64 MovingBaseService::nextSessionGeneration()
{
    ++m_nextSessionGeneration;
    if (m_nextSessionGeneration == 0) {
        ++m_nextSessionGeneration;
    }
    return m_nextSessionGeneration;
}
