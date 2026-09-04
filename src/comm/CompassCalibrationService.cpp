#include "CompassCalibrationService.h"

#include "VehicleCommandService.h"
#include "VehicleTargetManager.h"

#include <QPointer>

#include <cmath>

namespace {

constexpr quint8 kSuccessStatus = 4;
constexpr quint8 kFirstFailureStatus = 5;

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
    VehicleCommandService *commandService, QObject *parent)
    : QObject(parent)
    , m_targetManager(targetManager)
    , m_commandService(commandService)
{
    Q_ASSERT(m_targetManager);
    Q_ASSERT(m_commandService);
    qRegisterMetaType<CompassCalibrationService::State>(
        "CompassCalibrationService::State");
    qRegisterMetaType<CompassCalibrationService::RequestResult>(
        "CompassCalibrationService::RequestResult");

    for (QTimer *timer : {&m_commandTimer, &m_activityTimer, &m_totalTimer}) {
        timer->setSingleShot(true);
    }
    connect(&m_commandTimer, &QTimer::timeout,
            this, &CompassCalibrationService::handleCommandTimeout);
    connect(&m_activityTimer, &QTimer::timeout,
            this, &CompassCalibrationService::handleActivityTimeout);
    connect(&m_totalTimer, &QTimer::timeout,
            this, &CompassCalibrationService::handleTotalTimeout);

    connect(m_targetManager,
            &VehicleTargetManager::targetGenerationChanged,
            this, &CompassCalibrationService::handleTargetGenerationChanged);
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
    m_shuttingDown = true;
    ++m_operationToken;
    stopTimers();
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
        return true;
    default:
        return false;
    }
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
    if (m_poisonedGenerations.contains(lease.generation)) {
        m_resultText = tr(
            "This target generation has an uncertain calibration outcome. "
            "Cancel the onboard session or reconnect before retrying.");
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
    if (m_poisonedGenerations.contains(lease.generation)) {
        m_resultText = tr(
            "This target generation has an uncertain command outcome. "
            "Reconnect before retrying fixed-yaw calibration.");
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

void CompassCalibrationService::observeMessage(
    int linkId, const mavlink_message_t &message)
{
    if (m_shuttingDown || !m_lease.isValid()) {
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

void CompassCalibrationService::forgetLink(int linkId)
{
    if (!m_lease.isValid() || m_lease.endpoint.linkId != linkId) {
        return;
    }
    if (!isTerminalOperationState(m_state)) {
        transitionToUncertain(tr(
            "The physical link was lost during compass calibration."));
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
    m_shuttingDown = true;
    ++m_operationToken;
    stopTimers();
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
        && !m_poisonedGenerations.contains(lease.generation);
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
        return m_poisonedGenerations.contains(m_lease.generation)
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
        // armed: losing that terminal result poisons same-generation retry.
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
    if (m_pendingPurpose == CommandPurpose::None) {
        return;
    }
    transitionToUncertain(tr(
        "Compass calibration command acknowledgement timed out; the "
        "vehicle outcome is unknown."));
}

void CompassCalibrationService::handleActivityTimeout()
{
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
    if (m_state != State::StartPending
        && m_state != State::Running) {
        return;
    }
    appendResult(tr(
        "Compass calibration is taking longer than expected. It remains "
        "active and is not cancelled automatically."));
    emit changed();
}

void CompassCalibrationService::handleTargetGenerationChanged(
    qulonglong generation)
{
    if (!m_lease.isValid() || generation == m_lease.generation) {
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

    // The old generation remains poisoned, so its late ACK/telemetry cannot
    // complete anything. Release the UI binding for the fresh exact target;
    // known reboot requirements remain stored per endpoint.
    resetSession();
    m_state = State::Idle;
    m_resultText = tr(
        "Compass calibration state reset for the newly selected target.");
    emit changed();
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
        m_poisonedGenerations.insert(m_lease.generation);
    }
    m_state = State::OutcomeUncertain;
    appendResult(reason);
    emit changed();
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
    case State::Failed:
    case State::OutcomeUncertain:
        return true;
    default:
        return false;
    }
}
