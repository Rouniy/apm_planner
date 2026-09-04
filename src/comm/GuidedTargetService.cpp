#include "GuidedTargetService.h"

#include "VehicleCommandService.h"
#include "VehicleTargetManager.h"

#include <QThread>
#include <QtMath>

#include <cmath>
#include <limits>

#include <mavlink.h>

GuidedTargetService::GuidedTargetService(
    VehicleTargetManager *targetManager,
    VehicleCommandService *commandService,
    QObject *parent)
    : QObject(parent)
    , m_targetManager(targetManager)
    , m_commandService(commandService)
{
    qRegisterMetaType<GuidedTargetService::Target>();
    qRegisterMetaType<GuidedTargetService::SessionToken>();
    qRegisterMetaType<GuidedTargetService::State>();
    qRegisterMetaType<GuidedTargetService::RequestResult>();

    m_monotonicClock.start();
    m_commandTimer.setSingleShot(true);
    connect(&m_commandTimer, &QTimer::timeout,
            this, &GuidedTargetService::handleCommandTimeout);
    m_retryDrainTimer.setSingleShot(true);
    connect(&m_retryDrainTimer, &QTimer::timeout,
            this, &GuidedTargetService::handleRetryDrainExpired);
    m_quarantineTimer.setSingleShot(true);
    connect(&m_quarantineTimer, &QTimer::timeout,
            this, &GuidedTargetService::clearExpiredUncertainEndpoints);
    if (m_commandService) {
        connect(m_commandService, &VehicleCommandService::commandAckReceived,
                this, &GuidedTargetService::handleCommandAck);
    }
    if (m_targetManager) {
        connect(m_targetManager,
                &VehicleTargetManager::targetGenerationChanged,
                this, &GuidedTargetService::handleTargetGenerationChanged);
    }
}

GuidedTargetService::~GuidedTargetService()
{
    m_commandTimer.stop();
    m_retryDrainTimer.stop();
    m_quarantineTimer.stop();
    if (m_ownerDestroyedConnection) {
        disconnect(m_ownerDestroyedConnection);
    }
}

void GuidedTargetService::setLocalIdentity(
    quint8 systemId, quint8 componentId)
{
    if (systemId > 0) {
        m_localSystemId = systemId;
    }
    m_localComponentId = componentId;
}

void GuidedTargetService::setCommandTimeoutForTesting(int timeoutMs)
{
    m_commandTimeoutMs = qMax(1, timeoutMs);
    if (m_commandTimer.isActive()) {
        m_commandTimer.start(m_commandTimeoutMs);
    }
}

void GuidedTargetService::setRecoveryQuarantineForTesting(int timeoutMs)
{
    m_recoveryQuarantineMs = qMax(1, timeoutMs);
}

bool GuidedTargetService::isOutcomeUncertain(
    const VehicleEndpoint &endpoint) const
{
    if (!endpoint.isValid()) {
        return false;
    }
    const auto uncertain = m_uncertainEndpoints.constFind(endpoint);
    return uncertain != m_uncertainEndpoints.constEnd()
        && uncertain->expiresAtMs > m_monotonicClock.elapsed();
}

GuidedTargetService::RequestResult GuidedTargetService::start(
    QObject *owner, const VehicleTargetLease &target,
    const Target &initialTarget, SessionToken *sessionOut)
{
    if (sessionOut) {
        *sessionOut = SessionToken{};
    }
    if (!owner || owner == this || owner->thread() != thread()
        || QThread::currentThread() != thread()) {
        return RequestResult::InvalidOwner;
    }
    if (!isValidTarget(initialTarget)) {
        return RequestResult::InvalidPosition;
    }
    SessionToken reservedSession;
    const RequestResult reserved = reserve(owner, target, &reservedSession);
    if (reserved != RequestResult::Started) {
        return reserved;
    }

    const RequestResult sent = submit(reservedSession, initialTarget);
    if (sent != RequestResult::Sent) {
        return sent;
    }
    if (sessionOut && sessionMatches(reservedSession)) {
        *sessionOut = reservedSession;
    }
    return RequestResult::Started;
}

GuidedTargetService::RequestResult GuidedTargetService::reserve(
    QObject *owner, const VehicleTargetLease &target,
    SessionToken *sessionOut)
{
    if (sessionOut) {
        *sessionOut = SessionToken{};
    }
    if (!owner || owner == this || owner->thread() != thread()
        || QThread::currentThread() != thread()) {
        return RequestResult::InvalidOwner;
    }
    clearExpiredUncertainEndpoints();
    if (hasActiveSession()) {
        return RequestResult::Busy;
    }

    const RequestResult readiness = validateReadyTarget(target);
    if (readiness != RequestResult::Sent) {
        return readiness;
    }

    const quint64 sessionGeneration = nextSessionGeneration();
    if (sessionGeneration == 0) {
        return RequestResult::InvalidSession;
    }

    m_session.owner = owner;
    m_session.generation = sessionGeneration;
    m_session.target = target;
    m_firstAccepted = false;
    m_inFlight = false;
    m_hasQueuedTarget = false;
    m_sendAttempt = 0;
    m_ownerDestroyedConnection = connect(
        owner, &QObject::destroyed, this,
        [this, sessionGeneration]() {
            handleOwnerDestroyed(sessionGeneration);
        });

    m_state = State::Reserved;
    m_statusText = tr(
        "Guided channel reserved for the exact vehicle; waiting for the "
        "first valid target.");
    publishStateAndStatus();
    if (!hasActiveSession()
        || m_session.generation != sessionGeneration) {
        return m_lastFinishedSessionGeneration == sessionGeneration
            ? m_lastFinishedResult : RequestResult::InvalidSession;
    }
    if (sessionOut && m_session.generation == sessionGeneration) {
        *sessionOut = m_session;
    }
    return RequestResult::Started;
}

GuidedTargetService::RequestResult GuidedTargetService::submit(
    const SessionToken &session, const Target &target)
{
    if (!sessionMatches(session)) {
        return RequestResult::InvalidSession;
    }
    if (!isValidTarget(target)) {
        return RequestResult::InvalidPosition;
    }
    if (m_retryDrain) {
        m_queuedTarget = target;
        m_hasQueuedTarget = true;
        m_statusText = tr(
            "The accepted target is draining acknowledgements from retries; "
            "the newest file target is queued.");
        publishStateAndStatus();
        return RequestResult::Queued;
    }
    if (m_state == State::Draining
        || m_state == State::OutcomeUncertain) {
        return RequestResult::OutcomeUncertain;
    }
    if (!targetIsCurrent(m_session.target)) {
        if (m_inFlight) {
            finishWithUncertainOutcome(
                RequestResult::StaleTarget,
                tr("The selected vehicle changed while a guided command was "
                   "pending."),
                true);
        } else {
            finishSession(
                RequestResult::StaleTarget,
                tr("The selected vehicle changed; guided updates stopped."));
        }
        return RequestResult::StaleTarget;
    }

    if (m_inFlight) {
        m_queuedTarget = target;
        m_hasQueuedTarget = true;
        m_statusText = tr(
            "A guided command is awaiting acknowledgement; the newest target "
            "is queued.");
        publishStateAndStatus();
        return RequestResult::Queued;
    }
    return sendTarget(target);
}

GuidedTargetService::RequestResult GuidedTargetService::stop(
    const SessionToken &session)
{
    if (!sessionMatches(session)) {
        return RequestResult::InvalidSession;
    }
    if (m_retryDrain) {
        finishWithUncertainOutcome(
            RequestResult::Stopped,
            tr("Guided updates stopped during the retry acknowledgement "
               "drain. The window is stopped; this endpoint is held briefly "
               "before another session may start."),
            false);
        return RequestResult::Stopped;
    }
    if (m_inFlight) {
        finishWithUncertainOutcome(
            RequestResult::Stopped,
            tr("Guided updates stopped while a command was awaiting its "
               "terminal acknowledgement. The window is stopped; this "
               "endpoint is held briefly so a late acknowledgement cannot "
               "be mistaken for a new command."),
            true);
        return RequestResult::Stopped;
    }

    finishSession(RequestResult::Stopped,
                  tr("Guided updates stopped."));
    return RequestResult::Stopped;
}

void GuidedTargetService::forgetLink(int linkId)
{
    if (linkId < 0) {
        return;
    }

    for (auto endpoint = m_uncertainEndpoints.begin();
         endpoint != m_uncertainEndpoints.end();) {
        if (endpoint.key().linkId == linkId) {
            endpoint = m_uncertainEndpoints.erase(endpoint);
        } else {
            ++endpoint;
        }
    }
    scheduleUncertainEndpointExpiry();

    if (hasActiveSession()
        && m_session.target.endpoint.linkId == linkId) {
        finishSession(
            RequestResult::Stopped,
            tr("The physical link closed; its guided session was discarded."));
        return;
    }
    if (!hasActiveSession() && m_uncertainEndpoints.isEmpty()
        && m_state == State::OutcomeUncertain) {
        m_state = State::Idle;
        m_statusText = tr(
            "The closed physical link cleared the uncertain guided outcome.");
        publishStateAndStatus();
    }
}

bool GuidedTargetService::isValidTarget(const Target &target) noexcept
{
    return std::isfinite(target.latitude)
        && std::isfinite(target.longitude)
        && std::isfinite(target.relativeAltitudeM)
        && target.latitude >= -90.0 && target.latitude <= 90.0
        && target.longitude >= -180.0 && target.longitude <= 180.0
        && (target.latitude != 0.0 || target.longitude != 0.0)
        && target.relativeAltitudeM > 0.0
        && target.relativeAltitudeM <= 10000.0;
}

QString GuidedTargetService::stateDescription(State state)
{
    switch (state) {
    case State::Idle:
        return tr("Idle");
    case State::Reserved:
        return tr("Reserved; waiting for a target");
    case State::AwaitingAcknowledgement:
        return tr("Awaiting acknowledgement");
    case State::Active:
        return tr("Active");
    case State::Draining:
        return tr("Draining acknowledgements from identical retries");
    case State::OutcomeUncertain:
        return tr("Command outcome uncertain");
    }
    return tr("Unknown");
}

QString GuidedTargetService::resultDescription(RequestResult result)
{
    switch (result) {
    case RequestResult::Started:
        return tr("Guided session started.");
    case RequestResult::Sent:
        return tr("Guided target sent.");
    case RequestResult::Queued:
        return tr("Newest guided target queued.");
    case RequestResult::Stopped:
        return tr("Guided session stopped.");
    case RequestResult::Busy:
        return tr("Another window owns the guided command session.");
    case RequestResult::InvalidOwner:
        return tr("A live owner on the service thread is required.");
    case RequestResult::InvalidSession:
        return tr("The guided session token is no longer current.");
    case RequestResult::InvalidTarget:
        return tr("No valid exact vehicle target is selected.");
    case RequestResult::StaleTarget:
        return tr("The exact vehicle target changed.");
    case RequestResult::TargetUnsettled:
        return tr("The vehicle selection is still changing.");
    case RequestResult::HeartbeatStale:
        return tr("A fresh heartbeat from the exact vehicle is required.");
    case RequestResult::InvalidPosition:
        return tr("Latitude, longitude or relative altitude is invalid.");
    case RequestResult::TransportUnavailable:
        return tr("The exact physical-link transport is unavailable.");
    case RequestResult::OutcomeUncertain:
        return tr("A previous guided command outcome is uncertain.");
    case RequestResult::CommandRejected:
        return tr("The vehicle rejected the guided command.");
    case RequestResult::CommandUnsupported:
        return tr("The vehicle does not support the guided command.");
    case RequestResult::CommandCancelled:
        return tr("The vehicle cancelled the guided command.");
    case RequestResult::CommandFailed:
        return tr("The vehicle failed to execute the guided command.");
    }
    return tr("Unknown guided-command result.");
}

void GuidedTargetService::handleCommandAck(
    qulonglong targetGeneration,
    int linkId, int systemId, int componentId,
    int command, int result, int progress,
    int resultParam2, int targetSystem, int targetComponent)
{
    Q_UNUSED(resultParam2)
    Q_UNUSED(targetSystem)
    Q_UNUSED(targetComponent)

    if (command != MAV_CMD_DO_REPOSITION) {
        return;
    }

    VehicleEndpoint acknowledgementEndpoint;
    acknowledgementEndpoint.linkId = linkId;
    acknowledgementEndpoint.systemId = systemId;
    acknowledgementEndpoint.componentId = componentId;

    const bool matchesActive = hasActiveSession() && m_inFlight
        && targetGeneration == m_session.target.generation
        && acknowledgementEndpoint.sameIdentity(m_session.target.endpoint);
    if (!matchesActive) {
        const auto uncertain =
            m_uncertainEndpoints.constFind(acknowledgementEndpoint);
        if (uncertain == m_uncertainEndpoints.constEnd()
            || !uncertain->commandPending
            || uncertain->target.generation != targetGeneration
            || result == MAV_RESULT_IN_PROGRESS) {
            return;
        }
        clearUncertainEndpoint(acknowledgementEndpoint, targetGeneration);
        return;
    }

    if (result == MAV_RESULT_IN_PROGRESS) {
        if (m_state == State::AwaitingAcknowledgement) {
            m_commandTimer.start(m_commandTimeoutMs);
            m_statusText = tr(
                "The vehicle is processing the guided command (%1%).")
                .arg(progress);
            publishStateAndStatus();
        }
        return;
    }

    const SessionToken acknowledgedSession = m_session;
    const Target acknowledgedTarget = m_inFlightTarget;
    const bool hadQueuedTarget = m_hasQueuedTarget;
    const Target queuedTarget = m_queuedTarget;
    const State previousState = m_state;
    const int acknowledgedAttempts = m_sendAttempt;
    m_commandTimer.stop();
    m_inFlight = false;
    m_hasQueuedTarget = false;
    clearUncertainEndpoint(m_session.target.endpoint,
                           m_session.target.generation);

    if (previousState == State::Draining
        || previousState == State::OutcomeUncertain) {
        finishSession(
            RequestResult::Stopped,
            tr("A late terminal acknowledgement resolved the pending guided "
               "command; the session remains stopped."));
        return;
    }

    if (result == MAV_RESULT_ACCEPTED) {
        m_firstAccepted = true;
        if (acknowledgedAttempts > 1) {
            m_retryDrain = true;
            if (hadQueuedTarget) {
                m_queuedTarget = queuedTarget;
                m_hasQueuedTarget = true;
            }
            m_state = State::Draining;
            m_statusText = tr(
                "Guided target accepted after %1 send attempts; draining "
                "possible retry acknowledgements before the next target.")
                    .arg(acknowledgedAttempts);
            m_retryDrainTimer.start(m_recoveryQuarantineMs);
        } else {
            m_state = State::Active;
            m_statusText = tr(
                "Guided target accepted by the exact vehicle.");
        }
        m_sendAttempt = 0;
        publishStateAndStatus();
        if (!sessionMatches(acknowledgedSession)
            || (m_state != State::Active && !m_retryDrain)) {
            return;
        }
        emit targetAccepted(acknowledgedSession, acknowledgedTarget);
        if (!sessionMatches(acknowledgedSession)
            || (m_state != State::Active && !m_retryDrain)) {
            return;
        }
        if (hadQueuedTarget && !m_retryDrain) {
            sendTarget(queuedTarget);
        }
        return;
    }

    RequestResult terminalResult = RequestResult::CommandRejected;
    if (result == MAV_RESULT_UNSUPPORTED) {
        terminalResult = RequestResult::CommandUnsupported;
    } else if (result == MAV_RESULT_FAILED) {
        terminalResult = RequestResult::CommandFailed;
    } else if (result == 6) { // MAV_RESULT_CANCELLED in newer dialects.
        terminalResult = RequestResult::CommandCancelled;
    }
    const QString description = tr(
        "The vehicle returned terminal result %1 for DO_REPOSITION: %2")
        .arg(result).arg(resultDescription(terminalResult));
    if (acknowledgedAttempts > 1) {
        finishWithUncertainOutcome(terminalResult, description, false);
    } else {
        m_sendAttempt = 0;
        finishSession(terminalResult, description);
    }
}

void GuidedTargetService::handleTargetGenerationChanged(
    qulonglong generation)
{
    if (!hasActiveSession()
        || (generation == m_session.target.generation
            && targetIsCurrent(m_session.target))) {
        return;
    }
    if (m_inFlight || m_retryDrain) {
        finishWithUncertainOutcome(
            RequestResult::StaleTarget,
            tr("The selected vehicle changed while a guided command outcome "
               "was pending or draining. The old endpoint is held briefly so "
               "a late acknowledgement cannot be mistaken for a new command."),
            m_inFlight && m_sendAttempt <= 1);
    } else {
        finishSession(
            RequestResult::StaleTarget,
            tr("The selected vehicle changed; guided updates stopped."));
    }
}

void GuidedTargetService::handleCommandTimeout()
{
    if (!hasActiveSession() || !m_inFlight
        || m_state != State::AwaitingAcknowledgement) {
        return;
    }
    if (m_sendAttempt < MaximumSendAttempts) {
        const RequestResult resent = transmitInFlightTarget(true);
        if (resent == RequestResult::Sent) {
            return;
        }
        return;
    }

    finishWithUncertainOutcome(
        RequestResult::OutcomeUncertain,
        tr("The guided command received no terminal acknowledgement after %1 "
           "send attempts. Guided updates stopped; the endpoint is held "
           "briefly to isolate a late acknowledgement.")
            .arg(MaximumSendAttempts),
        true);
}

void GuidedTargetService::handleRetryDrainExpired()
{
    if (!hasActiveSession() || !m_retryDrain) {
        return;
    }
    const SessionToken session = m_session;
    const bool hadQueuedTarget = m_hasQueuedTarget;
    const Target queuedTarget = m_queuedTarget;
    m_retryDrain = false;
    m_hasQueuedTarget = false;
    m_state = State::Active;
    m_statusText = tr(
        "Retry acknowledgement drain completed; guided updates remain active.");
    publishStateAndStatus();
    if (!sessionMatches(session) || m_state != State::Active) {
        return;
    }
    if (hadQueuedTarget) {
        sendTarget(queuedTarget);
    }
}

bool GuidedTargetService::targetIsCurrent(
    const VehicleTargetLease &target) const
{
    return m_targetManager && target.isValid()
        && m_targetManager->isCurrentTarget(
            target.endpoint.linkId, target.endpoint.systemId,
            target.endpoint.componentId, target.generation);
}

bool GuidedTargetService::sessionMatches(
    const SessionToken &session) const
{
    return session.isValid() && hasActiveSession()
        && session.generation == m_session.generation
        && session.owner == m_session.owner
        && session.target.generation == m_session.target.generation
        && session.target.endpoint.sameIdentity(m_session.target.endpoint);
}

GuidedTargetService::RequestResult
GuidedTargetService::validateReadyTarget(
    const VehicleTargetLease &target)
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
    if (isOutcomeUncertain(target.endpoint)) {
        return RequestResult::OutcomeUncertain;
    }
    if (!m_targetManager->hasFreshHeartbeat(
            target, DefaultHeartbeatMaximumAgeMs)) {
        return RequestResult::HeartbeatStale;
    }
    if (!m_commandService) {
        return RequestResult::TransportUnavailable;
    }
    return RequestResult::Sent;
}

GuidedTargetService::RequestResult GuidedTargetService::sendTarget(
    const Target &target)
{
    if (!hasActiveSession()) {
        return RequestResult::InvalidSession;
    }
    if (!isValidTarget(target)) {
        return RequestResult::InvalidPosition;
    }
    const RequestResult readiness = validateReadyTarget(m_session.target);
    if (readiness != RequestResult::Sent) {
        finishSession(readiness, resultDescription(readiness));
        return readiness;
    }

    m_inFlightTarget = target;
    m_inFlight = true;
    m_sendAttempt = 0;
    return transmitInFlightTarget(false);
}

GuidedTargetService::RequestResult
GuidedTargetService::transmitInFlightTarget(bool retry)
{
    if (!hasActiveSession() || !m_inFlight) {
        return RequestResult::InvalidSession;
    }
    if (!m_commandService) {
        const QString description = tr(
            "The guided command transport disappeared while the command was "
            "pending.");
        if (retry) {
            finishWithUncertainOutcome(
                RequestResult::TransportUnavailable, description, false);
        } else {
            m_inFlight = false;
            finishSession(RequestResult::TransportUnavailable, description);
        }
        return RequestResult::TransportUnavailable;
    }
    if (retry) {
        if (!targetIsCurrent(m_session.target)) {
            finishWithUncertainOutcome(
                RequestResult::StaleTarget,
                tr("The exact vehicle target changed before the guided "
                   "command could be retried."),
                true);
            return RequestResult::StaleTarget;
        }
        if (!m_targetManager
            || !m_targetManager->hasFreshHeartbeat(
                m_session.target, DefaultHeartbeatMaximumAgeMs)) {
            finishWithUncertainOutcome(
                RequestResult::HeartbeatStale,
                tr("The exact vehicle heartbeat became stale before the "
                   "guided command could be retried."),
                true);
            return RequestResult::HeartbeatStale;
        }
    }

    ++m_sendAttempt;
    const quint64 sessionGeneration = m_session.generation;
    const VehicleEndpoint sessionEndpoint = m_session.target.endpoint;
    const quint64 transitionBeforeSend = m_transitionGeneration;
    m_state = State::AwaitingAcknowledgement;
    m_statusText = retry
        ? tr("Guided target retry %1 of %2 sent; awaiting vehicle "
             "acknowledgement.")
              .arg(m_sendAttempt).arg(MaximumSendAttempts)
        : tr("Guided target sent; awaiting vehicle acknowledgement.");
    m_commandTimer.start(m_commandTimeoutMs);

    QPointer<GuidedTargetService> guard(this);
    const VehicleCommandService::SendResult sent =
        m_commandService->sendCommandInt(
            m_session.target, m_localSystemId, m_localComponentId,
            MAV_CMD_DO_REPOSITION, MAV_FRAME_GLOBAL_RELATIVE_ALT,
            -1.0F,
            m_firstAccepted
                ? 0.0F
                : static_cast<float>(MAV_DO_REPOSITION_FLAGS_CHANGE_MODE),
            0.0F, std::numeric_limits<float>::quiet_NaN(),
            static_cast<qint32>(m_inFlightTarget.latitude * 1.0e7),
            static_cast<qint32>(m_inFlightTarget.longitude * 1.0e7),
            static_cast<float>(m_inFlightTarget.relativeAltitudeM));
    if (!guard) {
        return RequestResult::Sent;
    }
    if (m_session.generation != sessionGeneration) {
        if (m_lastFinishedSessionGeneration == sessionGeneration) {
            return m_lastFinishedResult;
        }
        return m_uncertainEndpoints.contains(sessionEndpoint)
            ? RequestResult::OutcomeUncertain : RequestResult::StaleTarget;
    }

    if (sent == VehicleCommandService::SendResult::Sent) {
        // A test transport, simulator or in-process bridge may deliver the ACK
        // synchronously from FrameWriter. Do not publish stale Awaiting state
        // after such a reentrant terminal transition.
        if (m_transitionGeneration == transitionBeforeSend) {
            publishStateAndStatus();
        }
        return RequestResult::Sent;
    }

    m_commandTimer.stop();
    if (sent == VehicleCommandService::SendResult::InvalidTarget) {
        if (m_sendAttempt > 1) {
            finishWithUncertainOutcome(
                RequestResult::InvalidTarget,
                resultDescription(RequestResult::InvalidTarget), true);
        } else {
            m_inFlight = false;
            finishSession(RequestResult::InvalidTarget,
                          resultDescription(RequestResult::InvalidTarget));
        }
        return RequestResult::InvalidTarget;
    }
    if (sent == VehicleCommandService::SendResult::StaleTarget) {
        finishWithUncertainOutcome(
            RequestResult::StaleTarget,
            resultDescription(RequestResult::StaleTarget),
            m_sendAttempt > 1);
        return RequestResult::StaleTarget;
    }

    // LinkManager may report failure after bytes entered the OS/device. The
    // generic command service rolls back its pending ACK entry in this case,
    // so the bounded recovery hold cannot be shortened by a terminal ACK.
    finishWithUncertainOutcome(
        RequestResult::TransportUnavailable,
        tr("The guided frame may have entered the physical transport, but its "
           "write outcome is unknown."),
        false);
    return RequestResult::TransportUnavailable;
}

void GuidedTargetService::handleOwnerDestroyed(
    quint64 sessionGeneration)
{
    if (!hasActiveSession()
        || m_session.generation != sessionGeneration) {
        return;
    }
    if (m_inFlight || m_retryDrain) {
        finishWithUncertainOutcome(
            RequestResult::Stopped,
            tr("The guided-session window was destroyed while a command was "
               "pending or draining. The owner was released and the endpoint "
               "is held briefly for late-acknowledgement isolation."),
            m_inFlight && m_sendAttempt <= 1);
    } else {
        finishSession(RequestResult::Stopped,
                      tr("The guided-session window was closed."));
    }
}

void GuidedTargetService::finishWithUncertainOutcome(
    RequestResult reason, const QString &description, bool commandPending)
{
    if (!hasActiveSession()) {
        return;
    }
    const VehicleTargetLease uncertainTarget = m_session.target;
    const bool newlyUncertain = !isOutcomeUncertain(
        uncertainTarget.endpoint);
    if (uncertainTarget.isValid()) {
        UncertainCommand uncertain;
        uncertain.target = uncertainTarget;
        uncertain.expiresAtMs = m_monotonicClock.elapsed()
            + m_recoveryQuarantineMs;
        uncertain.commandPending = commandPending && m_sendAttempt <= 1;
        m_uncertainEndpoints.insert(uncertainTarget.endpoint, uncertain);
        scheduleUncertainEndpointExpiry();
    }

    finishSession(reason, description, State::OutcomeUncertain);
    const quint64 transition = m_transitionGeneration;
    if (newlyUncertain) {
        emit outcomeBecameUncertain(uncertainTarget.endpoint, description);
        if (transition != m_transitionGeneration) {
            return;
        }
    }
}

void GuidedTargetService::clearExpiredUncertainEndpoints()
{
    const qint64 now = m_monotonicClock.elapsed();
    bool removed = false;
    for (auto endpoint = m_uncertainEndpoints.begin();
         endpoint != m_uncertainEndpoints.end();) {
        if (endpoint->expiresAtMs <= now) {
            endpoint = m_uncertainEndpoints.erase(endpoint);
            removed = true;
        } else {
            ++endpoint;
        }
    }
    scheduleUncertainEndpointExpiry();
    if (removed && m_uncertainEndpoints.isEmpty()
        && !hasActiveSession() && m_state == State::OutcomeUncertain) {
        m_state = State::Idle;
        m_statusText = tr(
            "The late-acknowledgement isolation interval expired; a new "
            "guided session may be started.");
        publishStateAndStatus();
    }
}

void GuidedTargetService::scheduleUncertainEndpointExpiry()
{
    if (m_uncertainEndpoints.isEmpty()) {
        m_quarantineTimer.stop();
        return;
    }
    qint64 nextExpiry = std::numeric_limits<qint64>::max();
    for (auto endpoint = m_uncertainEndpoints.constBegin();
         endpoint != m_uncertainEndpoints.constEnd(); ++endpoint) {
        nextExpiry = qMin(nextExpiry, endpoint->expiresAtMs);
    }
    const qint64 remaining = qMax<qint64>(
        1, nextExpiry - m_monotonicClock.elapsed());
    m_quarantineTimer.start(
        static_cast<int>(qMin<qint64>(remaining,
                                     std::numeric_limits<int>::max())));
}

void GuidedTargetService::clearUncertainEndpoint(
    const VehicleEndpoint &endpoint, quint64 targetGeneration)
{
    auto uncertain = m_uncertainEndpoints.find(endpoint);
    if (uncertain == m_uncertainEndpoints.end()
        || (targetGeneration != 0
            && uncertain->target.generation != targetGeneration)) {
        return;
    }
    m_uncertainEndpoints.erase(uncertain);
    scheduleUncertainEndpointExpiry();
    if (m_uncertainEndpoints.isEmpty()
        && !hasActiveSession() && m_state == State::OutcomeUncertain) {
        m_state = State::Idle;
        m_statusText = tr(
            "A terminal acknowledgement resolved the previous uncertain "
            "guided command.");
        publishStateAndStatus();
    }
}

void GuidedTargetService::finishSession(
    RequestResult reason, const QString &description, State finalState)
{
    const SessionToken endedSession = m_session;
    m_lastFinishedSessionGeneration = endedSession.generation;
    m_lastFinishedResult = reason;
    m_commandTimer.stop();
    resetSessionStorage();
    m_state = finalState;
    m_statusText = description;

    const quint64 transition = ++m_transitionGeneration;
    emit sessionEnded(endedSession, reason, description);
    if (transition != m_transitionGeneration) {
        return;
    }
    emit stateChanged(finalState);
    if (transition == m_transitionGeneration) {
        emit statusChanged(description);
    }
}

void GuidedTargetService::publishStateAndStatus()
{
    const State publishedState = m_state;
    const QString publishedStatus = m_statusText;
    const quint64 transition = ++m_transitionGeneration;
    emit stateChanged(publishedState);
    if (transition == m_transitionGeneration) {
        emit statusChanged(publishedStatus);
    }
}

void GuidedTargetService::resetSessionStorage()
{
    if (m_ownerDestroyedConnection) {
        disconnect(m_ownerDestroyedConnection);
        m_ownerDestroyedConnection = {};
    }
    m_session = SessionToken{};
    m_inFlight = false;
    m_hasQueuedTarget = false;
    m_firstAccepted = false;
    m_retryDrain = false;
    m_sendAttempt = 0;
    m_retryDrainTimer.stop();
    m_inFlightTarget = Target{};
    m_queuedTarget = Target{};
}

quint64 GuidedTargetService::nextSessionGeneration()
{
    if (m_nextSessionGeneration
        == std::numeric_limits<quint64>::max()) {
        return 0;
    }
    return ++m_nextSessionGeneration;
}
