#include "CompassCalibrationService.h"

#include "ExactLinkTransmitter.h"
#include "VehicleCommandService.h"
#include "VehicleTargetManager.h"

#include <QPointer>
#include <QTimer>

#include <cmath>
#include <cstring>

namespace {

constexpr quint8 kSuccessStatus = 4;
constexpr quint8 kFirstFailureStatus = 5;
constexpr int kMotorStopRepeatDelayMs = 20;

QString requestSendFailureText(
    VehicleCommandService::SendResult result)
{
    switch (result) {
    case VehicleCommandService::SendResult::InvalidTarget:
        return CompassCalibrationService::tr(
            "The compass calibration target is invalid.");
    case VehicleCommandService::SendResult::StaleTarget:
        return CompassCalibrationService::tr(
            "The selected compass calibration target changed.");
    case VehicleCommandService::SendResult::TransportUnavailable:
        return CompassCalibrationService::tr(
            "The exact vehicle link is unavailable.");
    case VehicleCommandService::SendResult::Sent:
        break;
    }
    return QString();
}

} // namespace

CompassCalibrationService::CompassCalibrationService(
    VehicleTargetManager *targetManager,
    VehicleCommandService *commandService,
    ExactLinkTransmitter *transmitter, QObject *parent)
    : QObject(parent)
    , m_targetManager(targetManager)
    , m_commandService(commandService)
    , m_transmitter(transmitter)
{
    Q_ASSERT(m_targetManager);
    Q_ASSERT(m_commandService);
    Q_ASSERT(m_transmitter);
    qRegisterMetaType<CompassCalibrationService::State>(
        "CompassCalibrationService::State");
    qRegisterMetaType<CompassCalibrationService::RequestResult>(
        "CompassCalibrationService::RequestResult");

    for (QTimer *timer : {&m_commandTimer, &m_activityTimer, &m_totalTimer,
                          &m_motorSettleTimer}) {
        timer->setSingleShot(true);
    }
    connect(&m_commandTimer, &QTimer::timeout,
            this, &CompassCalibrationService::handleCommandTimeout);
    connect(&m_activityTimer, &QTimer::timeout,
            this, &CompassCalibrationService::handleActivityTimeout);
    connect(&m_totalTimer, &QTimer::timeout,
            this, &CompassCalibrationService::handleTotalTimeout);
    connect(&m_motorSettleTimer, &QTimer::timeout,
            this, &CompassCalibrationService::handleMotorSettleTimeout);

    connect(m_targetManager,
            &VehicleTargetManager::targetGenerationChanged,
            this, &CompassCalibrationService::handleTargetGenerationChanged);
    connect(m_targetManager, &VehicleTargetManager::endpointsChanged,
            this, &CompassCalibrationService::handleEndpointRegistryChanged);
    connect(m_commandService,
            &VehicleCommandService::commandAckReceived,
            this,
            [this](qulonglong generation, int linkId, int systemId,
                   int componentId, int command, int result, int,
                   int, int, int) {
        handleCommandAck(generation, linkId, systemId, componentId,
                         command, result);
    });
}

CompassCalibrationService::~CompassCalibrationService()
{
    if (!m_shuttingDown) {
        attemptTeardownMotorStop();
    }
    m_shuttingDown = true;
    ++m_operationToken;
    stopTimers();
    m_motorSettleTimer.stop();
}

void CompassCalibrationService::setLocalIdentity(
    quint8 systemId, quint8 componentId)
{
    if (systemId > 0) {
        m_localSystemId = systemId;
    }
    m_localComponentId = componentId;
}

void CompassCalibrationService::setTimeoutsForTesting(
    int commandTimeoutMs, int activityTimeoutMs, int totalTimeoutMs)
{
    m_commandTimeoutMs = qMax(1, commandTimeoutMs);
    m_activityTimeoutMs = qMax(1, activityTimeoutMs);
    m_totalTimeoutMs = qMax(1, totalTimeoutMs);
}

void CompassCalibrationService::setMotorTimeoutsForTesting(
    int activityTimeoutMs, int totalTimeoutMs, int settleTimeoutMs)
{
    m_motorActivityTimeoutMs = qMax(1, activityTimeoutMs);
    m_motorTotalTimeoutMs = qMax(1, totalTimeoutMs);
    m_motorSettleTimeoutMs = qMax(1, settleTimeoutMs);
}

void CompassCalibrationService::setMotorHeartbeatTimeoutForTesting(
    int timeoutMs)
{
    m_motorHeartbeatTimeoutMs = qMax(1, timeoutMs);
}

int CompassCalibrationService::progress(int compassIndex) const
{
    return compassIndex >= 0
            && compassIndex < static_cast<int>(m_progress.size())
        ? m_progress.at(static_cast<size_t>(compassIndex)) : 0;
}

bool CompassCalibrationService::isOnboardActive() const
{
    switch (m_state) {
    case State::StartPending:
    case State::Running:
    case State::AwaitingAccept:
    case State::AcceptPending:
    case State::CancelPending:
        return true;
    case State::OutcomeUncertain:
    case State::Failed:
        return m_onboardRecoveryAvailable;
    default:
        return false;
    }
}

bool CompassCalibrationService::canCancel() const
{
    if (m_pendingPurpose != CommandPurpose::None) {
        return false;
    }
    switch (m_state) {
    case State::Running:
    case State::AwaitingAccept:
        return true;
    case State::Failed:
    case State::OutcomeUncertain:
        return m_onboardRecoveryAvailable && !m_cancelAttempted;
    default:
        return false;
    }
}

bool CompassCalibrationService::rebootRequiredFor(
    const VehicleTargetLease &lease) const
{
    return lease.isValid() && m_rebootTargets.contains(lease.endpoint);
}

bool CompassCalibrationService::isCurrentTarget(
    const VehicleTargetLease &lease) const
{
    return targetIsCurrent(lease);
}

bool CompassCalibrationService::isBusy() const
{
    switch (m_state) {
    case State::StartPending:
    case State::Running:
    case State::AcceptPending:
    case State::CancelPending:
    case State::FixedYawPending:
    case State::MotorStartPending:
    case State::MotorRunning:
    case State::MotorStopPending:
    case State::MotorStopSettling:
    case State::MotorOutcomeUncertain:
        return true;
    default:
        return false;
    }
}

bool CompassCalibrationService::blocksDeveloperTools(const VehicleTargetLease &target) const
{
    // A separate recovery command must not impersonate a calibration ACK or
    // bypass an endpoint whose prior calibration outcome remains uncertain.
    return isBusy() || isOnboardActive() || m_motorMayBeActive
        || (target.isValid() && m_poisonedEndpoints.contains(target.endpoint));
}

bool CompassCalibrationService::isMotorState(State state)
{
    switch (state) {
    case State::MotorStartPending:
    case State::MotorRunning:
    case State::MotorStopPending:
    case State::MotorStopSettling:
    case State::MotorSucceeded:
    case State::MotorFailed:
    case State::MotorCompletedUnverified:
    case State::MotorOutcomeUncertain:
        return true;
    default:
        return false;
    }
}

bool CompassCalibrationService::isMotorActive() const
{
    return m_state == State::MotorStartPending
        || m_state == State::MotorRunning
        || m_state == State::MotorStopPending
        || m_state == State::MotorStopSettling;
}

bool CompassCalibrationService::canStopMotor() const
{
    const bool firstAttempt = isMotorActive() && !m_motorStopAttempted;
    const bool explicitRetry = m_state == State::MotorOutcomeUncertain;
    return (firstAttempt || explicitRetry) && m_motorMayBeActive
        && m_lease.isValid() && m_transmitter;
}

bool CompassCalibrationService::canAcknowledgeMotorPowerDisconnected() const
{
    return m_state == State::MotorOutcomeUncertain
        && m_motorMayBeActive && m_lease.isValid() && m_targetManager
        && !m_targetManager->contains(
            m_lease.endpoint.linkId, m_lease.endpoint.systemId,
            m_lease.endpoint.componentId);
}

CompassCalibrationService::MotorSample
CompassCalibrationService::latestMotorSample() const
{
    return m_motorSamples.isEmpty() ? MotorSample{} : m_motorSamples.constLast();
}

bool CompassCalibrationService::linkHasSingleAutopilotTarget(
    const VehicleTargetLease &lease) const
{
    if (!m_targetManager || !lease.isValid()) {
        return false;
    }
    int autopilotTargets = 0;
    for (const VehicleEndpoint &endpoint : m_targetManager->endpoints()) {
        if (endpoint.linkId == lease.endpoint.linkId
            && endpoint.componentId == MAV_COMP_ID_AUTOPILOT1) {
            ++autopilotTargets;
        }
    }
    return autopilotTargets == 1;
}

bool CompassCalibrationService::hasSupportedMotorHeartbeat(
    const VehicleTargetLease &lease) const
{
    if (!m_targetManager
        || m_targetManager->heartbeatAutopilot(lease)
            != MAV_AUTOPILOT_ARDUPILOTMEGA) {
        return false;
    }
    switch (m_targetManager->heartbeatVehicleType(lease)) {
    case MAV_TYPE_TRICOPTER:
    case MAV_TYPE_QUADROTOR:
    case MAV_TYPE_COAXIAL:
    case MAV_TYPE_HEXAROTOR:
    case MAV_TYPE_OCTOROTOR:
    case MAV_TYPE_DODECAROTOR:
    case MAV_TYPE_DECAROTOR:
        return true;
    default:
        return false;
    }
}

bool CompassCalibrationService::supportsMotorCalibration(
    const VehicleTargetLease &lease) const
{
    return targetIsCurrent(lease) && m_transmitter
        && lease.endpoint.componentId == MAV_COMP_ID_AUTOPILOT1
        && m_targetManager->hasFreshHeartbeat(
            lease, m_motorHeartbeatTimeoutMs)
        && !m_targetManager->heartbeatArmed(lease)
        && hasSupportedMotorHeartbeat(lease)
        && m_transmitter->motorStopLinkEligible(lease.endpoint.linkId)
        && linkHasSingleAutopilotTarget(lease)
        && !rebootRequiredFor(lease)
        && !m_poisonedEndpoints.contains(lease.endpoint)
        && !m_motorEpochPoisonedEndpoints.contains(lease.endpoint);
}

bool CompassCalibrationService::blocksLegacyCalibrationMessage(
    int linkId, const mavlink_message_t &message) const
{
    if (!m_motorMayBeActive || !m_lease.isValid()
        || linkId != m_lease.endpoint.linkId) {
        return false;
    }
    if (message.msgid == MAVLINK_MSG_ID_COMMAND_ACK) {
        // ArduCopter's CompassMot loop stops on any incoming COMMAND_ACK,
        // regardless of its command/target fields.
        return true;
    }
    if (message.msgid != MAVLINK_MSG_ID_COMMAND_LONG) {
        return false;
    }
    mavlink_command_long_t command{};
    mavlink_msg_command_long_decode(&message, &command);
    return command.command == MAV_CMD_PREFLIGHT_CALIBRATION;
}

CompassCalibrationService::RequestResult
CompassCalibrationService::start(
    const VehicleTargetLease &lease, bool armed)
{
    if (m_shuttingDown) {
        return RequestResult::ShuttingDown;
    }
    if (armed) {
        m_resultText = tr(
            "Compass calibration is blocked while the vehicle is armed.");
        emit changed();
        return RequestResult::Armed;
    }
    if (!targetIsCurrent(lease)) {
        m_resultText = tr(
            "No current exact vehicle target is available for compass calibration.");
        emit changed();
        return RequestResult::InvalidTarget;
    }
    if (rebootRequiredFor(lease)) {
        m_resultText = tr(
            "Reboot the vehicle and refresh its state before calibrating.");
        emit changed();
        return RequestResult::RebootRequired;
    }
    if (m_poisonedEndpoints.contains(lease.endpoint)) {
        m_resultText = tr(
            "This physical endpoint has an uncertain calibration outcome. "
            "Complete any available onboard Cancel recovery, then restart "
            "APM Planner after confirming the vehicle is safe before retrying.");
        emit changed();
        return RequestResult::OutcomeUncertain;
    }
    if (m_state == State::OutcomeUncertain
        && m_lease.generation == lease.generation) {
        return RequestResult::OutcomeUncertain;
    }
    if (isBusy() || isOnboardActive()) {
        return RequestResult::Busy;
    }

    resetSession();
    m_lease = lease;
    m_state = State::Idle;
    m_onboardRecoveryAvailable = true;
    m_resultText = tr("Starting onboard magnetometer calibration...");
    m_totalTimer.start(m_totalTimeoutMs);
    return beginCommand(CommandPurpose::Start, State::StartPending,
                        MAV_CMD_DO_START_MAG_CAL,
                        0.0F, 1.0F, 1.0F, 0.0F,
                        0.0F, 0.0F, 0.0F);
}

CompassCalibrationService::RequestResult
CompassCalibrationService::accept(bool armed)
{
    if (m_shuttingDown) {
        return RequestResult::ShuttingDown;
    }
    if (m_state != State::AwaitingAccept
        || m_manualAcceptMask == 0) {
        return m_state == State::OutcomeUncertain
            ? RequestResult::OutcomeUncertain
            : RequestResult::InvalidState;
    }
    if (armed) {
        m_resultText = tr(
            "Compass calibration acceptance is blocked while the vehicle is armed.");
        emit changed();
        return RequestResult::Armed;
    }
    if (!targetIsCurrent(m_lease)) {
        transitionToUncertain(tr(
            "The exact target was lost before calibration acceptance."));
        return RequestResult::InvalidTarget;
    }

    // MP10 uses p3=1 even though the current MAVLink schema marks it empty.
    // p1 is narrowed to successful, unsaved sensors so a failed/retrying
    // compass can never be accepted accidentally.
    return beginCommand(CommandPurpose::Accept, State::AcceptPending,
                        MAV_CMD_DO_ACCEPT_MAG_CAL,
                        static_cast<float>(m_manualAcceptMask),
                        0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F);
}

CompassCalibrationService::RequestResult
CompassCalibrationService::cancel()
{
    if (m_shuttingDown) {
        return RequestResult::ShuttingDown;
    }
    if (!canCancel()) {
        return m_state == State::OutcomeUncertain
            ? RequestResult::OutcomeUncertain
            : RequestResult::InvalidState;
    }
    if (!targetIsCurrent(m_lease)) {
        transitionToUncertain(tr(
            "The exact target is unavailable; the onboard calibration "
            "could not be cancelled."));
        return RequestResult::InvalidTarget;
    }

    m_activityTimer.stop();
    m_totalTimer.stop();
    // Preserve MP10's command wire values: mask 0 means every compass.
    m_cancelAttempted = true;
    const RequestResult result = beginCommand(
        CommandPurpose::Cancel, State::CancelPending,
        MAV_CMD_DO_CANCEL_MAG_CAL,
        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F);
    if (result != RequestResult::Started) {
        m_cancelAttempted = false;
        if (m_state == State::Running) {
            m_activityTimer.start(m_activityTimeoutMs);
            m_totalTimer.start(m_totalTimeoutMs);
        }
    }
    return result;
}

CompassCalibrationService::RequestResult
CompassCalibrationService::fixedYaw(
    const VehicleTargetLease &lease, double headingDegrees, bool armed)
{
    if (m_shuttingDown) {
        return RequestResult::ShuttingDown;
    }
    if (armed) {
        m_resultText = tr(
            "Large-vehicle compass calibration is blocked while armed.");
        emit changed();
        return RequestResult::Armed;
    }
    if (!std::isfinite(headingDegrees)
        || headingDegrees < 0.0 || headingDegrees > 360.0) {
        m_resultText = tr(
            "The true heading must be a finite value from 0 to 360 degrees.");
        emit changed();
        return RequestResult::InvalidHeading;
    }
    if (!targetIsCurrent(lease)) {
        m_resultText = tr(
            "No current exact vehicle target is available for fixed-yaw calibration.");
        emit changed();
        return RequestResult::InvalidTarget;
    }
    if (rebootRequiredFor(lease)) {
        m_resultText = tr(
            "Reboot the vehicle before fixed-yaw compass calibration.");
        emit changed();
        return RequestResult::RebootRequired;
    }
    if (m_poisonedEndpoints.contains(lease.endpoint)) {
        m_resultText = tr(
            "This physical endpoint has an uncertain command outcome. "
            "Restart APM Planner after confirming the vehicle is safe before "
            "retrying fixed-yaw calibration.");
        emit changed();
        return RequestResult::OutcomeUncertain;
    }
    if (isBusy() || isOnboardActive()) {
        return RequestResult::Busy;
    }

    resetSession();
    m_lease = lease;
    m_resultText = tr("Submitting fixed-yaw compass calibration...");
    return beginCommand(CommandPurpose::FixedYaw,
                        State::FixedYawPending,
                        MAV_CMD_FIXED_MAG_CAL_YAW,
                        static_cast<float>(headingDegrees),
                        0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F);
}

CompassCalibrationService::RequestResult
CompassCalibrationService::startMotor(
    const VehicleTargetLease &lease, bool armed, bool dedicatedLinkConfirmed)
{
    // UAS state is legacy sysid-scoped and can refer to an equal sysid on a
    // different link. Exact heartbeat metadata below is authoritative; keep
    // the armed argument only for source compatibility during the migration.
    Q_UNUSED(armed)
    if (m_shuttingDown) {
        return RequestResult::ShuttingDown;
    }
    if (!targetIsCurrent(lease)) {
        m_resultText = tr(
            "No current exact vehicle target is available for "
            "Compass/Motor calibration.");
        emit changed();
        return RequestResult::InvalidTarget;
    }
    if (lease.endpoint.componentId != MAV_COMP_ID_AUTOPILOT1) {
        m_resultText = tr(
            "Compass/Motor calibration requires the autopilot component.");
        emit changed();
        return RequestResult::InvalidTarget;
    }
    if (!m_targetManager->hasFreshHeartbeat(
            lease, m_motorHeartbeatTimeoutMs)) {
        m_resultText = tr(
            "Compass/Motor calibration requires a fresh heartbeat from the "
            "exact autopilot; the cached armed state is not trusted.");
        emit changed();
        return RequestResult::HeartbeatStale;
    }
    if (!hasSupportedMotorHeartbeat(lease)) {
        m_resultText = tr(
            "Compass/Motor calibration is supported only for exact "
            "ArduCopter multirotor heartbeats (not Rover or helicopters).");
        emit changed();
        return RequestResult::UnsupportedVehicle;
    }
    if (m_targetManager->heartbeatArmed(lease)) {
        m_resultText = tr(
            "The exact autopilot heartbeat reports that the vehicle is armed. "
            "Disarm it before Compass/Motor calibration.");
        emit changed();
        return RequestResult::Armed;
    }
    if (!m_transmitter
        || !m_transmitter->motorStopLinkEligible(
            lease.endpoint.linkId)) {
        m_resultText = tr(
            "Compass/Motor calibration requires a point-to-point physical "
            "transport. Listening UDP links broadcast the unaddressable "
            "motor-stop acknowledgement to every peer.");
        emit changed();
        return RequestResult::UnsafeTransport;
    }
    if (!dedicatedLinkConfirmed) {
        m_resultText = tr(
            "Confirm that this is a dedicated direct connection to one "
            "autopilot, not a MAVLink router, radio network or multiplexed "
            "transport, before starting Compass/Motor calibration.");
        emit changed();
        return RequestResult::UnsafeTransport;
    }
    if (!linkHasSingleAutopilotTarget(lease)) {
        m_resultText = tr(
            "Compass/Motor calibration is blocked because this physical "
            "link carries more than one autopilot. Use a dedicated link.");
        emit changed();
        return RequestResult::SharedLinkUnsafe;
    }
    if (rebootRequiredFor(lease)) {
        m_resultText = tr(
            "Reboot the vehicle after onboard compass calibration before "
            "starting Compass/Motor calibration.");
        emit changed();
        return RequestResult::RebootRequired;
    }
    if (m_poisonedEndpoints.contains(lease.endpoint)
        || m_motorEpochPoisonedEndpoints.contains(lease.endpoint)
        || (m_state == State::MotorOutcomeUncertain
            && m_lease.generation == lease.generation)) {
        return RequestResult::OutcomeUncertain;
    }
    if (isBusy() || isOnboardActive()) {
        return RequestResult::Busy;
    }

    resetSession();
    m_lease = lease;
    m_state = State::MotorStartPending;
    m_motorMayBeActive = true;
    m_resultText = tr(
        "Compass/Motor start sent. Keep throttle at zero until the vehicle "
        "acknowledges the calibration.");
    appendMotorLog(tr("Starting Compass/Motor calibration..."));
    m_commandTimer.start(qMin(m_commandTimeoutMs,
                              m_motorActivityTimeoutMs));
    m_totalTimer.start(m_motorTotalTimeoutMs);

    const quint64 token = ++m_operationToken;
    QPointer<CompassCalibrationService> guard(this);
    const VehicleCommandService::SendResult sent =
        m_commandService->sendCommandLong(
            m_lease, m_localSystemId, m_localComponentId,
            MAV_CMD_PREFLIGHT_CALIBRATION, 0,
            0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F);
    if (!guard || token != m_operationToken
        || m_state != State::MotorStartPending) {
        return RequestResult::Started;
    }
    if (sent != VehicleCommandService::SendResult::Sent) {
        if (sent == VehicleCommandService::SendResult::InvalidTarget
            || sent == VehicleCommandService::SendResult::StaleTarget) {
            stopTimers();
            m_motorMayBeActive = false;
            m_state = State::MotorFailed;
            appendResult(requestSendFailureText(sent));
            emit changed();
            return RequestResult::InvalidTarget;
        }
        // writeRawBytes may return false after the frame already entered the
        // OS/device and the link disconnected reentrantly. Treat transport
        // failure as an ambiguous start and immediately attempt both stops.
        beginMotorStop(tr(
            "The motor-start write outcome is unknown. Emergency motor-stop "
            "acknowledgements are being attempted."));
        return RequestResult::TransportUnavailable;
    }

    emit changed();
    return RequestResult::Started;
}

CompassCalibrationService::RequestResult
CompassCalibrationService::stopMotor()
{
    if (m_shuttingDown) {
        return RequestResult::ShuttingDown;
    }
    if (m_state == State::MotorOutcomeUncertain
        && m_motorMayBeActive && m_lease.isValid()) {
        m_motorTerminalEvidence = -1;
        m_motorTerminalLine.clear();
        return beginMotorStop(tr(
            "Retrying the exact motor-stop acknowledgement. Keep clear of "
            "the vehicle until terminal evidence arrives."));
    }
    if (m_motorStopAttempted) {
        return m_state == State::MotorOutcomeUncertain
            ? RequestResult::OutcomeUncertain : RequestResult::Busy;
    }
    if (!m_motorMayBeActive || !isMotorState(m_state)
        || !m_lease.isValid()) {
        return RequestResult::InvalidState;
    }
    return beginMotorStop(tr(
        "Finishing Compass/Motor calibration and stopping all motors..."));
}

CompassCalibrationService::RequestResult
CompassCalibrationService::acknowledgeMotorPowerDisconnected()
{
    if (m_shuttingDown) {
        return RequestResult::ShuttingDown;
    }
    if (!canAcknowledgeMotorPowerDisconnected()) {
        return RequestResult::InvalidState;
    }
    const VehicleEndpoint unsafeEndpoint = m_lease.endpoint;
    resetSession();
    m_poisonedEndpoints.remove(unsafeEndpoint);
    m_motorEpochPoisonedEndpoints.remove(unsafeEndpoint);
    m_resultText = tr(
        "Unsafe Compass/Motor session cleared after power removal. "
        "Reconnect the vehicle before starting another calibration.");
    emit changed();
    return RequestResult::Started;
}

CompassCalibrationService::RequestResult
CompassCalibrationService::beginMotorStop(const QString &reason)
{
    if (!m_transmitter || !m_lease.isValid()) {
        transitionMotorToUncertain(tr(
            "The exact link is unavailable; motors may still be running."));
        return RequestResult::TransportUnavailable;
    }

    m_activityTimer.stop();
    m_totalTimer.stop();
    m_commandTimer.stop();
    m_motorSettleTimer.stop();
    m_motorStopAttempted = true;
    m_motorSecondStopPending = true;
    m_motorAnyStopFrameSent = false;
    m_motorRejectedStopAckSeen = false;
    m_motorFinalAckSeen = false;
    m_state = State::MotorStopPending;
    appendResult(reason);
    appendMotorLog(reason);
    const quint64 token = ++m_operationToken;

    m_motorAnyStopFrameSent = sendMotorStopFrame();
    if (!m_motorAnyStopFrameSent) {
        appendResult(tr(
            "The first motor-stop acknowledgement could not be confirmed; "
            "attempting the redundant frame."));
    }
    scheduleSecondMotorStopFrame(token);
    m_commandTimer.start(qMin(m_commandTimeoutMs,
                              m_motorActivityTimeoutMs));
    emit changed();
    return RequestResult::Started;
}

bool CompassCalibrationService::sendMotorStopFrame()
{
    if (!m_transmitter || !m_lease.isValid()) {
        return false;
    }

    const bool targeted = m_transmitter->supportsTargetedCommandAck(
        m_lease.endpoint.linkId);

    return m_transmitter->sendCommandAck(
        m_lease.endpoint.linkId, m_localSystemId, m_localComponentId,
        MAV_CMD_PREFLIGHT_CALIBRATION, MAV_RESULT_ACCEPTED,
        targeted ? static_cast<quint8>(m_lease.endpoint.systemId) : 0,
        targeted ? static_cast<quint8>(m_lease.endpoint.componentId) : 0)
        == ExactLinkTransmitter::SendResult::Sent;
}

void CompassCalibrationService::attemptTeardownMotorStop()
{
    if (!m_motorMayBeActive || !m_lease.isValid() || !m_transmitter) {
        return;
    }
    m_motorStopAttempted = true;
    if (m_motorSecondStopPending) {
        // Complete the already-started logical pair before the transport or
        // service disappears.
        sendMotorStopFrame();
    } else {
        // This includes MotorOutcomeUncertain and StopPending after its first
        // pair: teardown is the last bounded opportunity to retry safely.
        sendMotorStopFrame();
        sendMotorStopFrame();
    }
    m_motorSecondStopPending = false;
}

void CompassCalibrationService::scheduleSecondMotorStopFrame(quint64 token)
{
    QTimer::singleShot(kMotorStopRepeatDelayMs, this, [this, token]() {
        if (m_shuttingDown || token != m_operationToken
            || !m_motorStopAttempted || !m_motorSecondStopPending) {
            return;
        }
        const bool sent = sendMotorStopFrame();
        m_motorAnyStopFrameSent = m_motorAnyStopFrameSent || sent;
        m_motorSecondStopPending = false;
        if (!sent) {
            appendResult(tr(
                "The redundant motor-stop acknowledgement could not be sent; "
                "waiting for terminal vehicle evidence."));
        }
        if (!m_motorAnyStopFrameSent) {
            transitionMotorToUncertain(tr(
                "Neither exact motor-stop acknowledgement could be confirmed; "
                "disconnect power safely because motors may still be running."));
        } else if (m_motorTerminalEvidence >= 0) {
            finishMotorFromEvidence();
        } else if (m_motorRejectedStopAckSeen) {
            transitionMotorToUncertain(tr(
                "The stop result is ambiguous; disconnect power safely "
                "because motors may still be running."));
        } else {
            emit changed();
        }
    });
}

void CompassCalibrationService::observeMessage(
    int linkId, const mavlink_message_t &message)
{
    if (m_shuttingDown || !m_lease.isValid()) {
        return;
    }
    if (isMotorState(m_state)) {
        handleMotorMessage(linkId, message);
        return;
    }
    if (!targetIsCurrent(m_lease)) {
        if (!isTerminalOperationState(m_state)) {
            transitionToUncertain(tr(
                "The selected vehicle changed during compass calibration."));
        }
        return;
    }
    if (!acceptTelemetryEnvelope(linkId, message)
        || !m_startTelemetryBoundary) {
        // Telemetry arriving before an ACCEPTED/IN_PROGRESS start ACK is
        // deliberately quarantined. It cannot prove which start attempt owns
        // the packet and must never rescue StartPending.
        return;
    }
    if (m_state == State::CancelPending
        || m_state == State::CompletedNeedsReboot
        || m_state == State::Failed
        || m_state == State::OutcomeUncertain) {
        return;
    }

    if (message.msgid == MAVLINK_MSG_ID_MAG_CAL_PROGRESS) {
        handleProgress(message);
    } else if (message.msgid == MAVLINK_MSG_ID_MAG_CAL_REPORT) {
        handleReport(message);
    }
}

void CompassCalibrationService::handleMotorMessage(
    int linkId, const mavlink_message_t &message)
{
    if (!acceptMotorEnvelope(linkId, message)) {
        return;
    }
    switch (message.msgid) {
    case MAVLINK_MSG_ID_COMMAND_ACK:
        handleMotorAck(message);
        break;
    case MAVLINK_MSG_ID_COMPASSMOT_STATUS:
        handleMotorStatus(message);
        break;
    case MAVLINK_MSG_ID_STATUSTEXT:
        handleMotorStatusText(message);
        break;
    default:
        break;
    }
}

bool CompassCalibrationService::acceptMotorEnvelope(
    int linkId, const mavlink_message_t &message) const
{
    return m_lease.isValid()
        && linkId == m_lease.endpoint.linkId
        && message.sysid == m_lease.endpoint.systemId
        && message.compid == m_lease.endpoint.componentId
        && (message.msgid == MAVLINK_MSG_ID_COMMAND_ACK
            || message.msgid == MAVLINK_MSG_ID_COMPASSMOT_STATUS
            || message.msgid == MAVLINK_MSG_ID_STATUSTEXT);
}

void CompassCalibrationService::handleMotorAck(
    const mavlink_message_t &message)
{
    mavlink_command_ack_t acknowledgement{};
    mavlink_msg_command_ack_decode(&message, &acknowledgement);
    if (acknowledgement.command != MAV_CMD_PREFLIGHT_CALIBRATION
        || (acknowledgement.target_system != 0
            && acknowledgement.target_system != m_localSystemId)
        || (acknowledgement.target_component != 0
            && acknowledgement.target_component != m_localComponentId)) {
        return;
    }

    const int result = acknowledgement.result;
    const bool accepted = result == MAV_RESULT_ACCEPTED
        || result == MAV_RESULT_IN_PROGRESS;
    const bool canDistinguishAckPhase = m_transmitter
        && m_transmitter->supportsTargetedCommandAck(
            m_lease.endpoint.linkId);
    const bool addressedAck = acknowledgement.target_system != 0
        || acknowledgement.target_component != 0;
    const bool finalFormAck = canDistinguishAckPhase
        && acknowledgement.target_system == m_localSystemId
        && acknowledgement.target_component == m_localComponentId;
    if (m_state == State::MotorStartPending) {
        if (accepted && canDistinguishAckPhase && addressedAck) {
            // ArduCopter's live CompassMot loop sends an unaddressed manual
            // ACK. Its generic, addressed ACK is emitted only after the loop
            // returns, so this is a stale final response from an older epoch.
            appendMotorLog(tr(
                "Ignored an addressed final-form acknowledgement while "
                "waiting for the unaddressed Compass/Motor start ACK."));
            emit changed();
            return;
        }
        if (!accepted) {
            // TEMPORARILY_REJECTED explicitly means CompassMot may already be
            // running, and ACKs have no transaction id. Once Start was put on
            // the wire, no rejection proves the motor outputs are safe.
            beginMotorStop(tr(
                "The Compass/Motor start was rejected (MAV_RESULT %1), but "
                "motors may already be active. Emergency stop "
                "acknowledgements are being sent.").arg(result));
            return;
        }
        m_commandTimer.stop();
        m_motorStartAckSeen = true;
        m_motorStartWireAckSeen = true;
        m_state = State::MotorRunning;
        m_resultText = tr(
            "Compass/Motor calibration is active. Slowly raise throttle to "
            "maximum, then press Finish / Stop Motors.");
        appendMotorLog(tr("Vehicle accepted Compass/Motor calibration."));
        m_activityTimer.start(m_motorActivityTimeoutMs);
        emit changed();
        return;
    }

    if (m_state == State::MotorStopPending) {
        if (!accepted) {
            // Start and terminal ACKs share one command without a transaction
            // id. A rejected frame here can be a delayed start response and
            // cannot prove that firmware consumed either stop frame.
            m_motorRejectedStopAckSeen = true;
            const QString line = tr(
                "An ambiguous rejected calibration acknowledgement arrived "
                "while stopping (MAV_RESULT %1); motors are not considered "
                "stopped.").arg(result);
            appendResult(line);
            appendMotorLog(line);
            if (m_motorSecondStopPending) {
                emit changed();
            } else {
                transitionMotorToUncertain(tr(
                    "The stop result is ambiguous; disconnect power safely "
                    "because motors may still be running."));
            }
            return;
        }
        if (accepted && result != MAV_RESULT_IN_PROGRESS && finalFormAck
            && m_motorStartAckSeen) {
            // On MAVLink 2 this ACK is generated only after mavlink_compassmot
            // returns and disarms the motors. It is therefore a safe stop
            // boundary even if terminal STATUSTEXT was lost.
            m_commandTimer.stop();
            m_motorFinalAckSeen = true;
            m_motorMayBeActive = false;
            m_state = State::MotorStopSettling;
            if (m_motorTerminalEvidence >= 0) {
                finishMotorFromEvidence();
            } else {
                m_motorSettleTimer.start(m_motorSettleTimeoutMs);
                emit changed();
            }
            return;
        }
        if (accepted && result != MAV_RESULT_IN_PROGRESS && finalFormAck) {
            // The generic addressed ACK is a stop boundary only after this
            // process observed the current initial ACK or live status. A stale
            // terminal-text/final-ACK pair from an older epoch must not make a
            // just-sent Start look safe.
            m_commandTimer.start(qMin(m_commandTimeoutMs,
                                      m_motorActivityTimeoutMs));
            appendMotorLog(tr(
                "Ignored an addressed final acknowledgement without a "
                "correlated current Compass/Motor start boundary."));
            emit changed();
            return;
        }
        if (!m_motorStartWireAckSeen) {
            // A start timeout may have sent the emergency ACK before the
            // initial vehicle ACK arrived.  The first indistinguishable ACK
            // is only the start boundary; wait for terminal text or another
            // exact ACK instead of falsely declaring the motors stopped.
            m_motorStartAckSeen = true;
            m_motorStartWireAckSeen = true;
            m_commandTimer.start(qMin(m_commandTimeoutMs,
                                      m_motorActivityTimeoutMs));
            appendMotorLog(tr(
                "Late start acknowledgement received; waiting for stop "
                "confirmation."));
            emit changed();
            return;
        }
        if (result == MAV_RESULT_IN_PROGRESS) {
            m_commandTimer.start(qMin(m_commandTimeoutMs,
                                      m_motorActivityTimeoutMs));
            return;
        }
        if (canDistinguishAckPhase || m_motorTerminalEvidence < 0) {
            // Initial and final accepted ACKs are byte-identical. Firmware
            // emits its terminal STATUSTEXT before the final ACK. On MAVLink
            // 2 an unaddressed ACK is specifically the initial form; on
            // MAVLink 1 the terminal text is required before accepting it.
            m_commandTimer.start(qMin(m_commandTimeoutMs,
                                      m_motorActivityTimeoutMs));
            appendMotorLog(tr(
                "Ambiguous accepted acknowledgement received before the "
                "terminal result; still waiting for a proven stop."));
            emit changed();
            return;
        }
        m_commandTimer.stop();
        m_motorFinalAckSeen = true;
        m_motorMayBeActive = false;
        m_state = State::MotorStopSettling;
        if (m_motorTerminalEvidence >= 0) {
            finishMotorFromEvidence();
        } else {
            m_motorSettleTimer.start(m_motorSettleTimeoutMs);
            emit changed();
        }
        return;
    }

    if (m_state == State::MotorStopSettling) {
        if (accepted && result != MAV_RESULT_IN_PROGRESS
            && (!canDistinguishAckPhase || finalFormAck)) {
            m_motorFinalAckSeen = true;
            finishMotorFromEvidence();
        } else {
            // Terminal text already proves that the CompassMot loop exited,
            // but without its accepted final ACK a restart on this physical
            // endpoint remains unsafe because that late ACK has no transaction
            // id.
            m_motorSettleTimer.start(m_motorSettleTimeoutMs);
        }
        return;
    }

    if (m_state != State::MotorRunning) {
        return;
    }
    if (accepted && canDistinguishAckPhase && addressedAck) {
        // Never let an old addressed final ACK establish a new live epoch.
        return;
    }
    if (accepted && !m_motorStartWireAckSeen) {
        // COMPASSMOT_STATUS may have admitted the session before the initial
        // wire ACK arrived. Record this as the start ACK even when Finish is
        // already near; only a subsequent accepted ACK can be terminal.
        m_motorStartWireAckSeen = true;
        appendMotorLog(tr("Vehicle start acknowledgement received."));
        emit changed();
        return;
    }
    if (result == MAV_RESULT_IN_PROGRESS) {
        return;
    }
    if (!accepted) {
        // Live telemetry proves that motor passthrough started, so a later
        // conflicting rejection cannot make the vehicle safe. Initiate the
        // same bounded emergency stop as a telemetry watchdog.
        beginMotorStop(tr(
            "A conflicting calibration acknowledgement arrived while motors "
            "were active. An emergency motor-stop acknowledgement was sent."));
        return;
    }
    // Initial and final acknowledgements share the same command and result.
    // While no local Finish is pending, even an ACK after live status can be
    // a duplicate initial response. Exact terminal STATUSTEXT is the only
    // safe unsolicited-completion proof.
}

void CompassCalibrationService::handleMotorStatus(
    const mavlink_message_t &message)
{
    if (m_state == State::MotorStartPending) {
        // Unlike onboard MAG_CAL telemetry, an exact COMPASSMOT_STATUS frame
        // proves that motor passthrough is already active. Admit it as a
        // conservative start boundary even if the initial ACK was lost.
        m_commandTimer.stop();
        m_motorStartAckSeen = true;
        m_state = State::MotorRunning;
        m_resultText = tr(
            "Compass/Motor calibration is active (confirmed by telemetry). "
            "Slowly raise throttle, then press Finish / Stop Motors.");
        appendMotorLog(tr(
            "Live Compass/Motor telemetry confirmed the active session."));
    }
    if (!m_motorStartAckSeen
        || (m_state != State::MotorRunning
            && m_state != State::MotorStopPending)) {
        return;
    }

    mavlink_compassmot_status_t status{};
    mavlink_msg_compassmot_status_decode(&message, &status);
    if (!std::isfinite(status.current)
        || !std::isfinite(status.CompensationX)
        || !std::isfinite(status.CompensationY)
        || !std::isfinite(status.CompensationZ)
        || status.throttle > 1000U) {
        appendMotorLog(tr("Ignored an invalid Compass/Motor status sample."));
        emit changed();
        return;
    }

    MotorSample sample;
    sample.throttlePercent = status.throttle / 10.0;
    sample.currentAmps = status.current;
    sample.interferencePercent = qBound(
        0, static_cast<int>(status.interference), 100);
    sample.compensationX = status.CompensationX;
    sample.compensationY = status.CompensationY;
    sample.compensationZ = status.CompensationZ;
    if (m_motorSamples.size() >= MaximumMotorSamples) {
        m_motorSamples.removeFirst();
    }
    m_motorSamples.append(sample);
    m_motorStatusSeen = true;
    if (m_state == State::MotorRunning) {
        m_activityTimer.start(m_motorActivityTimeoutMs);
    }
    emit changed();
}

void CompassCalibrationService::handleMotorStatusText(
    const mavlink_message_t &message)
{
    if (!isMotorActive()
        && m_state != State::MotorOutcomeUncertain) {
        return;
    }
    mavlink_statustext_t status{};
    mavlink_msg_statustext_decode(&message, &status);
    const size_t length = strnlen(status.text, sizeof(status.text));
    const QString line = QString::fromUtf8(status.text,
                                            static_cast<int>(length)).trimmed();
    if (line.isEmpty()) {
        return;
    }
    appendMotorLog(line);

    int terminalEvidence = -1;
    if (line.contains(QStringLiteral("Calibration successful"),
                      Qt::CaseInsensitive)) {
        terminalEvidence = 1;
    } else if (line.compare(QStringLiteral("Failed"),
                            Qt::CaseInsensitive) == 0
               || line.contains(QStringLiteral("Calibration failed"),
                                Qt::CaseInsensitive)) {
        terminalEvidence = 0;
    }

    if (!m_motorStartAckSeen) {
        // No STATUSTEXT carries a transaction id. Without the unaddressed
        // initial ACK or live COMPASSMOT_STATUS boundary, even a terminal line
        // can belong to an older operation. Stop conservatively but do not use
        // the text to declare the motors safe.
        if (terminalEvidence >= 0 && !m_motorStopAttempted) {
            beginMotorStop(tr(
                "An uncorrelated terminal Compass/Motor message arrived. "
                "Emergency motor-stop acknowledgements are being sent."));
            return;
        }
        emit changed();
        return;
    }
    if (terminalEvidence >= 0) {
        m_motorTerminalEvidence = terminalEvidence;
        m_motorTerminalLine = line;
    }

    if (m_motorTerminalEvidence >= 0 && !m_motorStopAttempted) {
        // A terminal line can arrive before the start ACK or while the live
        // session ends unexpectedly. In both cases Start is already on the
        // wire, so send the normal redundant stop pair before accepting it.
        beginMotorStop(tr(
            "A terminal Compass/Motor result arrived unexpectedly. Exact "
            "motor-stop acknowledgements are being sent before completion."));
        return;
    }

    if (m_motorTerminalEvidence >= 0) {
        finishMotorFromEvidence();
    } else {
        emit changed();
    }
}

void CompassCalibrationService::finishMotorFromEvidence(
    bool allowMissingFinalAck)
{
    // A fast terminal STATUSTEXT/ACK can arrive before the deliberate 20 ms
    // redundant stop frame. Keep the operation busy until that second exact
    // frame is attempted, otherwise a fast restart could be stopped by a
    // stale scheduled ACK or the redundancy could be silently lost.
    if (m_motorStopAttempted && m_motorSecondStopPending) {
        emit changed();
        return;
    }
    if (!allowMissingFinalAck) {
        // Even with both evidence frames present, drain late duplicates before
        // another operation may start. Neither ACK nor STATUSTEXT carries a
        // transaction id that can distinguish the next CompassMot session.
        stopTimers();
        m_motorMayBeActive = false;
        m_state = State::MotorStopSettling;
        m_motorSettleTimer.start(m_motorSettleTimeoutMs);
        emit changed();
        return;
    }
    ++m_operationToken;
    stopTimers();
    m_motorSettleTimer.stop();
    m_motorMayBeActive = false;
    m_motorSecondStopPending = false;
    m_motorAnyStopFrameSent = false;
    if (m_lease.isValid()) {
        // CompassMot has no transaction id: even after terminal text plus a
        // final ACK, another delayed duplicate can impersonate the start ACK
        // of a new motor attempt. Block only CompassMot on this physical
        // endpoint; ordinary onboard/fixed-yaw commands remain safe.
        m_poisonedEndpoints.remove(m_lease.endpoint);
        m_motorEpochPoisonedEndpoints.insert(m_lease.endpoint);
    }
    if (m_motorTerminalEvidence == 1) {
        m_state = State::MotorSucceeded;
        appendResult(tr(
            "Compass/Motor calibration succeeded and the vehicle reported "
            "that compensation was stored."));
    } else {
        m_state = State::MotorFailed;
        appendResult(tr(
            "Compass/Motor calibration stopped without a usable sample; "
            "motor compensation was not enabled."));
    }
    emit changed();
}

void CompassCalibrationService::finishMotorUnverified()
{
    ++m_operationToken;
    stopTimers();
    m_motorSettleTimer.stop();
    m_motorMayBeActive = false;
    m_motorSecondStopPending = false;
    if (m_lease.isValid()) {
        // Missing terminal text can arrive late and must not be attributed to
        // another motor attempt on the same physical endpoint.
        m_poisonedEndpoints.remove(m_lease.endpoint);
        m_motorEpochPoisonedEndpoints.insert(m_lease.endpoint);
    }
    m_state = State::MotorCompletedUnverified;
    appendResult(tr(
        "The vehicle acknowledged completion, but no exact success/failure "
        "message arrived. Stored compensation is unverified."));
    emit changed();
}

void CompassCalibrationService::appendMotorLog(const QString &line)
{
    if (line.trimmed().isEmpty()) {
        return;
    }
    constexpr int maximumLines = 100;
    while (m_motorLog.size() >= maximumLines) {
        m_motorLog.removeFirst();
    }
    m_motorLog.append(line.trimmed());
}

void CompassCalibrationService::forgetLink(int linkId)
{
    if (!m_lease.isValid() || m_lease.endpoint.linkId != linkId) {
        return;
    }
    if (m_motorMayBeActive) {
        // LinkManager calls forgetLink before removing the exact transport.
        // Use that last safe window even after an earlier uncertain outcome.
        attemptTeardownMotorStop();
        transitionMotorToUncertain(tr(
            "The physical link was lost during Compass/Motor calibration; "
            "motors may still be running. Disconnect power safely."));
    } else if (!isTerminalOperationState(m_state)) {
        transitionToUncertain(tr(
            "The physical link was lost during compass calibration."));
    } else {
        // A removed transport is an actual epoch boundary. Safe terminal motor
        // sessions no longer need their late-ACK drain latch; uncertainty uses
        // m_poisonedEndpoints and still requires explicit power confirmation.
        m_motorEpochPoisonedEndpoints.remove(m_lease.endpoint);
    }
}

void CompassCalibrationService::forgetLink()
{
    if (m_lease.isValid()) {
        forgetLink(m_lease.endpoint.linkId);
    }
}

void CompassCalibrationService::clearRebootRequired(
    const VehicleTargetLease &lease)
{
    if (!lease.isValid() || !m_rebootTargets.remove(lease.endpoint)) {
        return;
    }
    if (m_state == State::CompletedNeedsReboot
        && (!m_lease.isValid()
            || m_lease.endpoint.sameIdentity(lease.endpoint))) {
        resetSession();
        m_state = State::Idle;
        m_resultText = tr("Vehicle reboot acknowledged; calibration is ready.");
    }
    emit changed();
}

void CompassCalibrationService::shutdown()
{
    if (m_shuttingDown) {
        return;
    }
    attemptTeardownMotorStop();
    m_shuttingDown = true;
    ++m_operationToken;
    stopTimers();
    m_motorSettleTimer.stop();
    m_motorSecondStopPending = false;
    m_pendingPurpose = CommandPurpose::None;
    m_pendingCommand = static_cast<MAV_CMD>(0);
    m_startTelemetryBoundary = false;
    m_onboardRecoveryAvailable = false;
    m_lease = VehicleTargetLease();
    m_rebootTargets.clear();
    m_state = State::Idle;
    m_resultText = tr("Compass calibration service stopped.");
    emit changed();
}

bool CompassCalibrationService::targetIsCurrent(
    const VehicleTargetLease &lease) const
{
    return m_targetManager && lease.isValid()
        && m_targetManager->isCurrentTarget(
            lease.endpoint.linkId, lease.endpoint.systemId,
            lease.endpoint.componentId, lease.generation);
}

bool CompassCalibrationService::mayBeginForLease(
    const VehicleTargetLease &lease) const
{
    return !m_shuttingDown && targetIsCurrent(lease)
        && !m_poisonedEndpoints.contains(lease.endpoint);
}

CompassCalibrationService::RequestResult
CompassCalibrationService::beginCommand(
    CommandPurpose purpose, State pendingState, MAV_CMD command,
    float p1, float p2, float p3, float p4,
    float p5, float p6, float p7)
{
    if (m_shuttingDown || !m_commandService) {
        return m_shuttingDown ? RequestResult::ShuttingDown
                              : RequestResult::TransportUnavailable;
    }
    if (!mayBeginForLease(m_lease)
        && !(purpose == CommandPurpose::Cancel
             && targetIsCurrent(m_lease))) {
        return m_poisonedEndpoints.contains(m_lease.endpoint)
            ? RequestResult::OutcomeUncertain
            : RequestResult::InvalidTarget;
    }

    const quint64 token = ++m_operationToken;
    m_stateBeforeCommand = m_state;
    m_state = pendingState;
    m_pendingPurpose = purpose;
    m_pendingCommand = command;
    m_commandTimer.start(m_commandTimeoutMs);

    QPointer<CompassCalibrationService> guard(this);
    const VehicleCommandService::SendResult sendResult =
        m_commandService->sendCommandLong(
            m_lease, m_localSystemId, m_localComponentId,
            command, 0, p1, p2, p3, p4, p5, p6, p7);
    if (!guard) {
        return RequestResult::Started;
    }
    // A synchronous transport callback may have completed the operation or a
    // state observer may have shut the service down. Never overwrite it.
    if (m_operationToken != token
        || m_pendingPurpose != purpose
        || m_pendingCommand != command) {
        return RequestResult::Started;
    }
    if (sendResult != VehicleCommandService::SendResult::Sent) {
        m_commandTimer.stop();
        m_pendingPurpose = CommandPurpose::None;
        m_pendingCommand = static_cast<MAV_CMD>(0);
        const QString reason = requestSendFailureText(sendResult);
        if (purpose == CommandPurpose::Cancel) {
            m_cancelAttempted = false;
        }
        if (purpose == CommandPurpose::Start
            || purpose == CommandPurpose::FixedYaw) {
            m_onboardRecoveryAvailable = false;
            transitionToFailed(reason);
        } else {
            m_state = m_stateBeforeCommand;
            appendResult(reason);
            emit changed();
        }
        return sendResult == VehicleCommandService::SendResult::StaleTarget
            || sendResult == VehicleCommandService::SendResult::InvalidTarget
            ? RequestResult::InvalidTarget
            : RequestResult::TransportUnavailable;
    }

    emit changed();
    return RequestResult::Started;
}

void CompassCalibrationService::handleCommandAck(
    qulonglong generation, int linkId, int systemId, int componentId,
    int command, int result)
{
    if (m_shuttingDown || m_pendingPurpose == CommandPurpose::None
        || !m_lease.isValid()
        || generation != m_lease.generation
        || linkId != m_lease.endpoint.linkId
        || systemId != m_lease.endpoint.systemId
        || componentId != m_lease.endpoint.componentId
        || command != static_cast<int>(m_pendingCommand)) {
        return;
    }
    if (!targetIsCurrent(m_lease)) {
        transitionToUncertain(tr(
            "A command acknowledgement arrived after the target changed."));
        return;
    }

    if (result == MAV_RESULT_IN_PROGRESS) {
        if (m_pendingPurpose == CommandPurpose::Start) {
            m_startTelemetryBoundary = true;
            m_state = State::Running;
            m_activityTimer.start(m_activityTimeoutMs);
        }
        // COMMAND_ACK IN_PROGRESS is sufficient to admit exact telemetry, but
        // MAVLink still promises a later terminal ACK. Keep the command timer
        // armed: losing that terminal result poisons this physical endpoint.
        m_commandTimer.start(m_commandTimeoutMs);
        emit changed();
        return;
    }

    m_commandTimer.stop();
    const CommandPurpose purpose = m_pendingPurpose;
    const MAV_CMD acknowledgedCommand = m_pendingCommand;
    m_pendingPurpose = CommandPurpose::None;
    m_pendingCommand = static_cast<MAV_CMD>(0);

    if (result != MAV_RESULT_ACCEPTED) {
        const bool preserveRecovery = purpose == CommandPurpose::Accept
            || purpose == CommandPurpose::Cancel;
        if (purpose == CommandPurpose::Start) {
            m_onboardRecoveryAvailable = false;
        } else if (purpose == CommandPurpose::Cancel) {
            // A terminal rejection is unambiguous and permits one new
            // recovery attempt. Only a lost cancellation ACK is poisoned.
            m_cancelAttempted = false;
        }
        transitionToFailed(
            commandFailureText(acknowledgedCommand, result),
            preserveRecovery);
        return;
    }

    switch (purpose) {
    case CommandPurpose::Start:
        m_startTelemetryBoundary = true;
        m_state = State::Running;
        m_resultText = tr(
            "Onboard magnetometer calibration started. Move the vehicle "
            "through all orientations.");
        m_activityTimer.start(m_activityTimeoutMs);
        evaluateReports();
        emit changed();
        return;
    case CommandPurpose::Accept:
        stopTimers();
        m_state = State::CompletedNeedsReboot;
        m_rebootTargets.insert(m_lease.endpoint);
        m_onboardRecoveryAvailable = false;
        appendResult(tr("Calibration accepted. Please reboot the vehicle."));
        emit changed();
        return;
    case CommandPurpose::Cancel:
        finishCancel();
        return;
    case CommandPurpose::FixedYaw:
        stopTimers();
        m_state = State::FixedYawCompleted;
        m_resultText = tr("Fixed-yaw compass calibration completed.");
        emit changed();
        return;
    case CommandPurpose::None:
        return;
    }
}

void CompassCalibrationService::handleProgress(
    const mavlink_message_t &message)
{
    mavlink_mag_cal_progress_t progressMessage{};
    mavlink_msg_mag_cal_progress_decode(&message, &progressMessage);
    if (progressMessage.cal_status <= 1) {
        return;
    }
    if (!acceptCalibrationMask(progressMessage.compass_id,
                               progressMessage.cal_mask)) {
        return;
    }

    m_activityTimer.start(m_activityTimeoutMs);
    if (progressMessage.compass_id < m_progress.size()) {
        m_progress[progressMessage.compass_id] = qBound(
            0, static_cast<int>(progressMessage.completion_pct), 100);
    }
    emit changed();
}

void CompassCalibrationService::handleReport(
    const mavlink_message_t &message)
{
    mavlink_mag_cal_report_t report{};
    mavlink_msg_mag_cal_report_decode(&message, &report);
    // MP10 and the original WinForms implementation both discard this empty
    // sentinel. Treating it as a real result can falsely complete or poison a
    // fresh calibration before firmware has produced a measurement.
    if ((report.compass_id == 0 && report.ofs_x == 0.0F)
        || report.cal_status <= 1) {
        return;
    }
    if (!acceptCalibrationMask(report.compass_id, report.cal_mask)) {
        return;
    }

    m_activityTimer.start(m_activityTimeoutMs);
    const quint8 compassId = report.compass_id;
    m_reportStatus[compassId] = report.cal_status;
    m_reportAutosaved[compassId] = report.autosaved == 1;

    if (report.cal_status == kSuccessStatus) {
        m_reportSeen[compassId] = true;
        if (compassId < m_progress.size()) {
            m_progress[compassId] = 100;
        }
        if (report.autosaved == 1) {
            m_rebootTargets.insert(m_lease.endpoint);
        }
    } else {
        // START requests firmware retry-on-failure. A failure report is the
        // result of one attempt, not terminal proof for the whole session.
        // A later success report for the same compass may supersede it.
        m_reportSeen[compassId] = false;
        if (report.cal_status >= kFirstFailureStatus
            && compassId < m_progress.size()) {
            m_progress[compassId] = 0;
        }
    }

    const QString reportLine = QStringLiteral(
        "id:%1 x:%2 y:%3 z:%4 fit:%5 %6%7")
        .arg(compassId)
        .arg(report.ofs_x, 0, 'f', 1)
        .arg(report.ofs_y, 0, 'f', 1)
        .arg(report.ofs_z, 0, 'f', 1)
        .arg(report.fitness, 0, 'f', 1)
        .arg(calibrationStatusText(report.cal_status))
        .arg(report.autosaved == 1 ? tr(" (saved)") : QString());
    if (m_lastReportLines[compassId] != reportLine) {
        m_lastReportLines[compassId] = reportLine;
        appendResult(reportLine);
    }

    if (m_state == State::AcceptPending
        && report.cal_status != kSuccessStatus) {
        transitionToUncertain(tr(
            "Calibration status changed while acceptance was pending."));
        return;
    }
    evaluateReports();
    emit changed();
}

bool CompassCalibrationService::acceptTelemetryEnvelope(
    int linkId, const mavlink_message_t &message) const
{
    return (message.msgid == MAVLINK_MSG_ID_MAG_CAL_PROGRESS
            || message.msgid == MAVLINK_MSG_ID_MAG_CAL_REPORT)
        && linkId == m_lease.endpoint.linkId
        && message.sysid == m_lease.endpoint.systemId
        && message.compid == m_lease.endpoint.componentId;
}

bool CompassCalibrationService::acceptCalibrationMask(
    quint8 compassId, quint8 mask)
{
    if (compassId >= 8 || mask == 0
        || (mask & static_cast<quint8>(1U << compassId)) == 0) {
        // Stale and placeholder frames have no operation token. Ignore an
        // envelope that cannot describe this instance; only a conflicting
        // valid running mask is evidence that our session is ambiguous.
        return false;
    }
    if (m_calibrationMask == 0) {
        m_calibrationMask = mask;
        return true;
    }
    if (mask != m_calibrationMask) {
        transitionToUncertain(tr(
            "The compass calibration mask changed from 0x%1 to 0x%2.")
            .arg(m_calibrationMask, 2, 16, QLatin1Char('0'))
            .arg(mask, 2, 16, QLatin1Char('0')));
        return false;
    }
    return true;
}

void CompassCalibrationService::evaluateReports()
{
    if (m_calibrationMask == 0
        || m_pendingPurpose == CommandPurpose::Start) {
        return;
    }

    bool allSuccessful = true;
    quint8 unsavedMask = 0;
    for (int compassId = 0; compassId < 8; ++compassId) {
        const quint8 bit = static_cast<quint8>(1U << compassId);
        if ((m_calibrationMask & bit) == 0) {
            continue;
        }
        if (!m_reportSeen[static_cast<size_t>(compassId)]
            || m_reportStatus[static_cast<size_t>(compassId)]
                   != kSuccessStatus) {
            allSuccessful = false;
            break;
        }
        if (!m_reportAutosaved[static_cast<size_t>(compassId)]) {
            unsavedMask = static_cast<quint8>(unsavedMask | bit);
        }
    }

    if (!allSuccessful) {
        m_manualAcceptMask = 0;
        if (m_state == State::AwaitingAccept) {
            m_state = State::Running;
            m_totalTimer.start(m_totalTimeoutMs);
            m_activityTimer.start(m_activityTimeoutMs);
        }
        return;
    }
    if (m_pendingPurpose == CommandPurpose::Accept) {
        return;
    }

    m_manualAcceptMask = unsavedMask;
    m_activityTimer.stop();
    m_totalTimer.stop();
    if (unsavedMask != 0) {
        m_state = State::AwaitingAccept;
        appendResult(tr(
            "Calibration succeeded; accept the unsaved compass results."));
        return;
    }

    m_state = State::CompletedNeedsReboot;
    m_rebootTargets.insert(m_lease.endpoint);
    m_onboardRecoveryAvailable = false;
    appendResult(tr("Calibration saved. Please reboot the vehicle."));
}

void CompassCalibrationService::handleCommandTimeout()
{
    if (m_state == State::MotorStartPending) {
        beginMotorStop(tr(
            "The start acknowledgement timed out. An emergency motor-stop "
            "acknowledgement was sent; waiting for terminal evidence."));
        return;
    }
    if (m_state == State::MotorStopPending) {
        transitionMotorToUncertain(tr(
            "No terminal evidence followed the motor-stop acknowledgement. "
            "Motors may still be running; disconnect power safely."));
        return;
    }
    if (m_pendingPurpose == CommandPurpose::None) {
        return;
    }
    transitionToUncertain(tr(
        "Compass calibration command acknowledgement timed out; the "
        "vehicle outcome is unknown."));
}

void CompassCalibrationService::handleActivityTimeout()
{
    if (m_state == State::MotorRunning) {
        handleMotorActivityTimeout();
        return;
    }
    if (m_state != State::Running) {
        return;
    }
    appendResult(tr(
        "No compass calibration telemetry was received recently. The vehicle "
        "calibration remains active; use Cancel if it should be stopped."));
    emit changed();
}

void CompassCalibrationService::handleTotalTimeout()
{
    if (m_state == State::MotorStartPending
        || m_state == State::MotorRunning) {
        handleMotorTotalTimeout();
        return;
    }
    if (m_state != State::StartPending
        && m_state != State::Running) {
        return;
    }
    appendResult(tr(
        "Compass calibration is taking longer than expected. It remains "
        "active and is not cancelled automatically."));
    emit changed();
}

void CompassCalibrationService::handleMotorActivityTimeout()
{
    if (m_state != State::MotorRunning || m_motorStopAttempted) {
        return;
    }
    beginMotorStop(tr(
        "Compass/Motor telemetry stopped. An emergency motor-stop "
        "acknowledgement was sent."));
}

void CompassCalibrationService::handleMotorTotalTimeout()
{
    if ((m_state != State::MotorStartPending
         && m_state != State::MotorRunning)
        || m_motorStopAttempted) {
        return;
    }
    beginMotorStop(tr(
        "Compass/Motor calibration exceeded its safety time limit. An "
        "emergency motor-stop acknowledgement was sent."));
}

void CompassCalibrationService::handleMotorSettleTimeout()
{
    if (m_state == State::MotorStopSettling
        && m_motorTerminalEvidence >= 0) {
        finishMotorFromEvidence(true);
    } else if (m_state == State::MotorStopSettling) {
        finishMotorUnverified();
    }
}

void CompassCalibrationService::handleTargetGenerationChanged(
    qulonglong generation)
{
    if (!m_lease.isValid() || generation == m_lease.generation) {
        return;
    }
    if (isMotorActive()) {
        if (!m_motorStopAttempted) {
            beginMotorStop(tr(
                "The selected target changed. A stop acknowledgement was sent "
                "to the pinned Compass/Motor vehicle."));
        }
        // Keep the pinned lease until its exact terminal evidence arrives.
        // Never re-target an in-flight motor operation to the new selection.
        return;
    }
    if (m_state == State::MotorOutcomeUncertain) {
        return;
    }
    if (!isTerminalOperationState(m_state)) {
        transitionToUncertain(tr(
            "The selected vehicle changed during compass calibration."));
    }

    const VehicleTargetLease current = m_targetManager
        ? m_targetManager->acquireTarget() : VehicleTargetLease();
    if (!current.isValid() || current.generation != generation) {
        // Keep the orphaned lease visible until a new exact target exists.
        return;
    }

    // The old endpoint remains poisoned when its outcome was uncertain, so
    // away/back target churn cannot admit late ACK/telemetry. Release the UI
    // binding for the fresh exact target; known reboot requirements remain
    // stored per endpoint.
    resetSession();
    m_state = State::Idle;
    m_resultText = tr(
        "Compass calibration state reset for the newly selected target.");
    emit changed();
}

void CompassCalibrationService::handleEndpointRegistryChanged()
{
    if (!isMotorActive() || !m_motorMayBeActive || !m_lease.isValid()
        || linkHasSingleAutopilotTarget(m_lease)
        || m_motorStopAttempted) {
        return;
    }
    beginMotorStop(tr(
        "Another autopilot appeared on the pinned physical link. Emergency "
        "motor-stop acknowledgements are being sent; do not start another "
        "calibration on this shared transport."));
}

void CompassCalibrationService::transitionToFailed(
    const QString &reason, bool preserveOnboardRecovery)
{
    ++m_operationToken;
    stopTimers();
    m_pendingPurpose = CommandPurpose::None;
    m_pendingCommand = static_cast<MAV_CMD>(0);
    m_startTelemetryBoundary = false;
    m_manualAcceptMask = 0;
    m_onboardRecoveryAvailable = preserveOnboardRecovery
        && m_onboardRecoveryAvailable;
    m_state = State::Failed;
    appendResult(reason);
    emit changed();
}

void CompassCalibrationService::transitionToUncertain(
    const QString &reason)
{
    ++m_operationToken;
    stopTimers();
    m_pendingPurpose = CommandPurpose::None;
    m_pendingCommand = static_cast<MAV_CMD>(0);
    m_startTelemetryBoundary = false;
    m_manualAcceptMask = 0;
    if (m_lease.isValid()) {
        m_poisonedEndpoints.insert(m_lease.endpoint);
    }
    m_state = State::OutcomeUncertain;
    appendResult(reason);
    emit changed();
}

void CompassCalibrationService::transitionMotorToUncertain(
    const QString &reason)
{
    ++m_operationToken;
    stopTimers();
    m_motorSettleTimer.stop();
    m_motorSecondStopPending = false;
    m_pendingPurpose = CommandPurpose::None;
    m_pendingCommand = static_cast<MAV_CMD>(0);
    if (m_lease.isValid()) {
        m_poisonedEndpoints.insert(m_lease.endpoint);
    }
    m_state = State::MotorOutcomeUncertain;
    appendResult(reason);
    appendMotorLog(reason);
    emit changed();
    emit motorSafetyWarning(reason);
}

void CompassCalibrationService::finishCancel()
{
    const bool rebootRequired = rebootRequiredFor(m_lease);
    resetSession();
    m_state = State::Idle;
    m_resultText = rebootRequired
        ? tr("Onboard calibration cancelled. Previously saved changes still "
             "require a reboot.")
        : tr("Onboard compass calibration cancelled.");
    emit changed();
}

void CompassCalibrationService::resetSession()
{
    ++m_operationToken;
    stopTimers();
    m_lease = VehicleTargetLease();
    m_state = State::Idle;
    m_stateBeforeCommand = State::Idle;
    m_pendingPurpose = CommandPurpose::None;
    m_pendingCommand = static_cast<MAV_CMD>(0);
    m_calibrationMask = 0;
    m_manualAcceptMask = 0;
    m_progress.fill(0);
    m_reportStatus.fill(0);
    m_reportSeen.fill(false);
    m_reportAutosaved.fill(false);
    m_lastReportLines.fill(QString());
    m_resultText.clear();
    m_startTelemetryBoundary = false;
    m_onboardRecoveryAvailable = false;
    m_cancelAttempted = false;
    m_motorSettleTimer.stop();
    m_motorSamples.clear();
    m_motorLog.clear();
    m_motorMayBeActive = false;
    m_motorStartAckSeen = false;
    m_motorStartWireAckSeen = false;
    m_motorStatusSeen = false;
    m_motorStopAttempted = false;
    m_motorSecondStopPending = false;
    m_motorAnyStopFrameSent = false;
    m_motorRejectedStopAckSeen = false;
    m_motorFinalAckSeen = false;
    m_motorTerminalEvidence = -1;
    m_motorTerminalLine.clear();
}

void CompassCalibrationService::stopTimers()
{
    m_commandTimer.stop();
    m_activityTimer.stop();
    m_totalTimer.stop();
}

void CompassCalibrationService::appendResult(const QString &line)
{
    if (line.isEmpty()) {
        return;
    }
    if (!m_resultText.isEmpty()) {
        m_resultText.append(QLatin1Char('\n'));
    }
    m_resultText.append(line);
}

QString CompassCalibrationService::calibrationStatusText(quint8 status)
{
    // The bundled dialect predates ArduPilot's status values 8..10. Keep the
    // raw wire mapping here until the repository-wide MAVLink update lands.
    switch (status) {
    case 0: return tr("MAG_CAL_NOT_STARTED");
    case 1: return tr("MAG_CAL_WAITING_TO_START");
    case 2: return tr("MAG_CAL_RUNNING_STEP_ONE");
    case 3: return tr("MAG_CAL_RUNNING_STEP_TWO");
    case 4: return tr("MAG_CAL_SUCCESS");
    case 5: return tr("MAG_CAL_FAILED");
    case 6: return tr("MAG_CAL_FAILED_ORIENTATION");
    case 7: return tr("MAG_CAL_FAILED_RADIUS");
    case 8: return tr("MAG_CAL_FAILED_OFFSETS");
    case 9: return tr("MAG_CAL_FAILED_DIAG_SCALING");
    case 10: return tr("MAG_CAL_FAILED_RESIDUALS_HIGH");
    default:
        return tr("MAG_CAL_STATUS(%1)").arg(status);
    }
}

QString CompassCalibrationService::commandFailureText(
    MAV_CMD command, int result)
{
    return tr("Compass calibration command %1 was rejected "
              "(MAV_RESULT %2).")
        .arg(static_cast<int>(command)).arg(result);
}

bool CompassCalibrationService::isTerminalOperationState(State state)
{
    switch (state) {
    case State::Idle:
    case State::CompletedNeedsReboot:
    case State::FixedYawCompleted:
    case State::MotorSucceeded:
    case State::MotorFailed:
    case State::MotorCompletedUnverified:
    case State::MotorOutcomeUncertain:
    case State::Failed:
    case State::OutcomeUncertain:
        return true;
    default:
        return false;
    }
}
