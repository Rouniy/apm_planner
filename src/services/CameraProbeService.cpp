#include "CameraProbeService.h"
#include "comm/MavlinkComponentRegistry.h"
#include "comm/VehicleTargetManager.h"
#include <QTimer>
#include <algorithm>
#include <utility>

namespace {
struct ScopeExit {
    std::function<void()> action;
    ~ScopeExit() { action(); }
};
}

CameraProbeService::CameraProbeService(VehicleTargetManager *targets,
    MavlinkComponentRegistry *components, VehicleCommandService *commands,
    RouteValidator validateRoute, QObject *parent)
    : QObject(parent), m_targets(targets), m_components(components),
      m_commands(commands), m_validateRoute(std::move(validateRoute))
{
    if (targets) connect(targets, &VehicleTargetManager::targetGenerationChanged,
                         this, &CameraProbeService::invalidateSource);
    if (components) connect(components, &MavlinkComponentRegistry::componentRetired,
        this, [this](const MavlinkComponentInstanceLease &lease) {
        if (m_prepared.camera == lease || (m_busy && m_report.plan.camera == lease))
            invalidateSource();
    });
    if (commands) connect(commands, &VehicleCommandService::exactCommandFinished,
                          this, &CameraProbeService::commandFinished);
}

CameraProbeService::~CameraProbeService()
{
    m_shuttingDown = true;
    if (m_commands) {
        disconnect(m_commands, nullptr, this, nullptr);
        m_commands->releaseExactReservation(m_reservation);
    }
}

QList<MAV_CMD> CameraProbeService::Commands()
{
    return {MAV_CMD_REQUEST_CAMERA_INFORMATION, MAV_CMD_REQUEST_VIDEO_STREAM_INFORMATION,
            MAV_CMD_REQUEST_CAMERA_SETTINGS, MAV_CMD_SET_CAMERA_MODE,
            MAV_CMD_REQUEST_STORAGE_INFORMATION, MAV_CMD_VIDEO_START_STREAMING};
}

QString CameraProbeService::CommandName(MAV_CMD command)
{
    switch (command) {
    case MAV_CMD_REQUEST_CAMERA_INFORMATION: return QStringLiteral("REQUEST_CAMERA_INFORMATION (521)");
    case MAV_CMD_REQUEST_VIDEO_STREAM_INFORMATION: return QStringLiteral("REQUEST_VIDEO_STREAM_INFORMATION (2504)");
    case MAV_CMD_REQUEST_CAMERA_SETTINGS: return QStringLiteral("REQUEST_CAMERA_SETTINGS (522)");
    case MAV_CMD_SET_CAMERA_MODE: return QStringLiteral("SET_CAMERA_MODE (530)");
    case MAV_CMD_REQUEST_STORAGE_INFORMATION: return QStringLiteral("REQUEST_STORAGE_INFORMATION (525)");
    case MAV_CMD_VIDEO_START_STREAMING: return QStringLiteral("VIDEO_START_STREAMING (2502)");
    default: return QString::number(int(command));
    }
}

QString CameraProbeService::ConfirmationText(const Plan &plan)
{
    return tr("Request information, settings and storage status from camera %1:%2 on link %3, "
              "select its default camera mode and request video streaming?\n\n"
              "This probe changes camera state: mode 0 (image) and video stream 0 (all streams) are requested. "
              "The six commands retain Mission Planner's all-zero parameters; some modern cameras "
              "treat the legacy information requests as no action.\n\n"
              "The first discovered camera on this link is frozen for this confirmation "
              "(physical epoch %4, component instance %5). The selected autopilot is not changed. "
              "Each command may be sent up to four times, two seconds apart. "
              "In-progress replies are bounded to 30 seconds per command. "
              "Cancel stops remaining requests; already applied mode or streaming changes are not undone.\n\n"
              "Acknowledgement does not prove that camera data or video arrived. "
              "Inspect responses in MAVLink Inspector.")
        .arg(plan.camera.endpoint.systemId).arg(plan.camera.endpoint.componentId)
        .arg(plan.camera.endpoint.linkId).arg(plan.camera.linkSessionEpoch)
        .arg(plan.camera.instanceEpoch);
}

void CameraProbeService::setTimeoutsForTesting(int acknowledgementMs, int maximumLifetimeMs)
{
    if (busy()) return;
    m_acknowledgementMs = qBound(1, acknowledgementMs, 2000);
    m_maximumLifetimeMs = qBound(m_acknowledgementMs, maximumLifetimeMs, 30000);
}

bool CameraProbeService::samePlan(const Plan &left, const Plan &right) const
{
    return left.isValid() && right.isValid() && left.planId == right.planId
        && left.selection.generation == right.selection.generation
        && left.selection.endpoint == right.selection.endpoint && left.camera == right.camera;
}

bool CameraProbeService::validateIdentity(const Plan &plan, QString *error) const
{
    auto fail = [error](const QString &reason) { if (error) *error = reason; return false; };
    if (!m_targets || !m_components || !m_commands || !m_validateRoute)
        return fail(tr("Camera probe services are unavailable."));
    if (!plan.selection.isValid() || !plan.camera.isValid()
        || plan.camera.endpoint.componentId != MAV_COMP_ID_CAMERA
        || plan.camera.endpoint.linkId != plan.selection.endpoint.linkId)
        return fail(tr("The camera probe target is invalid."));
    const auto selected = m_targets->acquireTarget();
    if (!m_targets->isTargetGenerationSettled()
        || selected.generation != plan.selection.generation
        || selected.endpoint != plan.selection.endpoint)
        return fail(tr("The selected modem or vehicle changed; review the camera target again."));
    QPointer<CameraProbeService> guard(const_cast<CameraProbeService *>(this));
    const bool live = m_components->validateLease(plan.camera);
    if (!guard) return false;
    if (!live) return fail(tr("The consented camera is no longer present in this physical session."));
    const auto route = m_validateRoute;
    QString reason;
    const bool routed = route(plan.camera, &reason);
    if (!guard) return false;
    if (!routed) return fail(reason.isEmpty() ? tr("The camera route is unavailable.") : reason);
    if (!m_targets || !m_components || !m_commands) return false;
    const auto finalTarget = m_targets->acquireTarget();
    if (!m_targets->isTargetGenerationSettled()
        || finalTarget.generation != plan.selection.generation
        || finalTarget.endpoint != plan.selection.endpoint
        || !m_components->validateLease(plan.camera))
        return fail(tr("The selected source or camera changed during validation."));
    return true;
}

bool CameraProbeService::capture(Plan *plan, QString *error) const
{
    QPointer<CameraProbeService> guard(const_cast<CameraProbeService *>(this));
    if (!m_targets || !m_components || !m_commands || !m_targets->hasCurrentTarget()) {
        if (error) *error = tr("Camera probe: connect and select a modem or vehicle first.");
        return false;
    }
    Plan candidate;
    candidate.selection = m_targets->acquireTarget();
    // MP10 uses Dictionary.Values.FirstOrDefault without an explicit order.
    // Preserve first-known intent deterministically, not sorted system ID.
    const auto cameras = m_components->components();
    if (!guard) return false;
    for (const auto &camera : cameras) {
        if (camera.endpoint.componentId == MAV_COMP_ID_CAMERA
            && camera.endpoint.linkId == candidate.selection.endpoint.linkId
            && (!candidate.camera.isValid() || camera.instanceEpoch < candidate.camera.instanceEpoch))
            candidate.camera = camera;
    }
    if (!candidate.camera.isValid()) {
        if (error) *error = tr("Camera probe: no MAV_COMP_ID_CAMERA component is currently known on this link.");
        return false;
    }
    const bool valid = validateIdentity(candidate, error);
    if (!guard || !valid) return false;
    *plan = candidate;
    return true;
}

bool CameraProbeService::prepare(Plan *plan, QString *error)
{
    if (plan) *plan = Plan();
    if (error) error->clear();
    if (!plan || busy() || m_apiInFlight || m_shuttingDown) {
        if (error) *error = tr("Camera probe is busy or unavailable.");
        return false;
    }
    QPointer<CameraProbeService> guard(this);
    m_apiInFlight = true;
    ScopeExit api{[guard] { if (guard) guard->m_apiInFlight = false; }};
    const quint64 revision = ++m_revision;
    m_prepared = Plan();
    Plan captured;
    const bool ready = capture(&captured, error);
    if (!guard || revision != m_revision || !ready) return false;
    captured.planId = ++m_nextPlan;
    m_prepared = captured;
    *plan = captured;
    return true;
}

bool CameraProbeService::validate(const Plan &plan, QString *error) const
{
    if (error) error->clear();
    const Plan frozen = plan;
    if (busy() || m_shuttingDown || !samePlan(frozen, m_prepared)) {
        if (error) *error = tr("The camera confirmation is stale; prepare it again.");
        return false;
    }
    QPointer<CameraProbeService> guard(const_cast<CameraProbeService *>(this));
    const quint64 revision = m_revision;
    const bool valid = validateIdentity(frozen, error);
    return guard && revision == m_revision && valid && samePlan(frozen, m_prepared);
}

bool CameraProbeService::execute(const Plan &plan, quint64 *operationIdOut, QString *error)
{
    if (operationIdOut) *operationIdOut = 0;
    if (error) error->clear();
    const Plan requested = plan;
    if (busy() || m_apiInFlight || m_shuttingDown || !samePlan(requested, m_prepared)) {
        if (error) *error = tr("The camera confirmation is stale, or a probe is already active.");
        return false;
    }
    QPointer<CameraProbeService> guard(this);
    const QPointer<VehicleCommandService> commands = m_commands;
    m_apiInFlight = true;
    ScopeExit api{[guard] { if (guard) guard->m_apiInFlight = false; }};
    const quint64 revision = ++m_revision;
    m_report = Report();
    m_report.plan = m_prepared;
    m_prepared = Plan();
    m_report.operationId = ++m_nextOperation;
    for (MAV_CMD command : Commands()) { StepResult step; step.command = command; m_report.steps.append(step); }
    m_step = 0;
    m_cancelRequested = false;
    m_stopReason.clear();
    m_busy = true;
    if (operationIdOut) *operationIdOut = m_report.operationId;
    emit stateChanged();
    if (!guard || revision != m_revision || !m_busy) return false;
    QString reason;
    const bool valid = validateIdentity(m_report.plan, &reason);
    if (!guard || revision != m_revision || !m_busy) return false;
    if (!valid || !commands) {
        if (error) *error = reason;
        finish(reason.isEmpty() ? tr("Camera command service is unavailable.") : reason);
        return false;
    }
    VehicleCommandService::ExactReservationToken reservation;
    const auto reserved = commands->reserveComponentEndpoint(this, m_report.plan.camera, &reservation, &reason);
    if (!guard || revision != m_revision || !m_busy) {
        if (commands && reservation.isValid()) commands->releaseExactReservation(reservation);
        return false;
    }
    if (reserved != VehicleCommandService::ExactReservationResult::Reserved) {
        if (error) *error = reason;
        finish(reason.isEmpty() ? tr("The camera command lane is unavailable.") : reason);
        return false;
    }
    m_reservation = reservation;
    appendLog(tr("Camera probe: six requests prepared for %1:%2 on link %3.")
        .arg(m_report.plan.camera.endpoint.systemId).arg(m_report.plan.camera.endpoint.componentId)
        .arg(m_report.plan.camera.endpoint.linkId));
    if (!guard || revision != m_revision) return false;
    QTimer::singleShot(0, this, [this, revision] { nextStep(revision); });
    return true;
}

void CameraProbeService::nextStep(quint64 revision)
{
    if (revision != m_revision || !m_busy || m_cancelRequested || m_command.isValid()) return;
    if (m_step >= m_report.steps.size()) {
        finish(tr("Camera probe finished. Command acknowledgements are listed above; inspect camera responses in MAVLink Inspector. No stream or camera-data arrival is implied."));
        return;
    }
    QPointer<CameraProbeService> guard(this);
    const QPointer<VehicleCommandService> commands = m_commands;
    const Plan frozen = m_report.plan;
    QString error;
    const bool valid = validateIdentity(frozen, &error);
    if (!guard || revision != m_revision || !m_busy) return;
    if (!valid || !commands) { finish(error.isEmpty() ? tr("Camera command service is unavailable.") : error); return; }
    VehicleCommandService::ExactCommandRequest request;
    request.command = m_report.steps[m_step].command;
    request.acknowledgementTimeoutMs = m_acknowledgementMs;
    request.maximumLifetimeMs = m_maximumLifetimeMs;
    request.maximumRetries = 3;
    request.validateBeforeWrite = [guard, revision, frozen](QString *reason) {
        if (!guard || guard->m_revision != revision || !guard->m_busy || guard->m_cancelRequested) {
            if (reason) *reason = QStringLiteral("Camera probe was cancelled or replaced.");
            return false;
        }
        const bool current = guard->validateIdentity(frozen, reason);
        return guard && guard->m_revision == revision && guard->m_busy
            && !guard->m_cancelRequested && current;
    };
    const auto result = commands->submitComponentCommandLong(m_reservation, frozen.camera,
        request, &m_command, &error);
    if (!guard || revision != m_revision || !m_busy) return;
    if (result != VehicleCommandService::ExactSubmitResult::Started) {
        // A synchronous terminal report may already have consumed the token
        // and queued the next step; never overwrite that authoritative receipt.
        if (!m_command.isValid()) {
            if (result == VehicleCommandService::ExactSubmitResult::TransportOutcomeUncertain) return;
            finish(error.isEmpty() ? tr("Camera command submission failed.") : error);
        }
    }
}

void CameraProbeService::commandFinished(const VehicleCommandService::ExactCommandReport &report)
{
    if (!m_busy || !m_command.isValid() || !report.token.isComponentOperation()
        || report.token.transactionId != m_command.transactionId
        || report.token.reservationId != m_reservation.reservationId
        || report.token.componentLease != m_report.plan.camera
        || report.token.command != m_report.steps[m_step].command) return;
    const auto terminal = report.terminalResult;
    auto &step = m_report.steps[m_step];
    step.attempts = report.transmissionAttempts;
    step.mavResult = report.mavResult;
    step.description = report.description;
    step.outcome = terminal == VehicleCommandService::ExactTerminalResult::AcknowledgedAccepted
        ? StepOutcome::Accepted
        : terminal == VehicleCommandService::ExactTerminalResult::AcknowledgedRejected
            ? StepOutcome::Rejected
            : report.frameAttempted ? StepOutcome::OutcomeUncertain : StepOutcome::NotSent;
    m_command = VehicleCommandService::ExactCommandToken();
    const bool definite = step.outcome == StepOutcome::Accepted || step.outcome == StepOutcome::Rejected;
    const QString text = tr("%1/6 %2 — %3 (transmission attempts: %4).")
        .arg(m_step + 1).arg(CommandName(step.command)).arg(step.description).arg(step.attempts);
    QPointer<CameraProbeService> guard(this);
    const quint64 revision = m_revision;
    ++m_step;
    appendLog(text);
    if (!guard || revision != m_revision || !m_busy) return;
    emit stateChanged();
    if (!guard || revision != m_revision || !m_busy) return;
    if (m_cancelRequested) { finish(m_stopReason, true); return; }
    if (!definite) { finish(tr("Camera probe stopped: the last request was not acknowledged or could not be sent. Remaining requests were not sent; camera effects may be uncertain.")); return; }
    QTimer::singleShot(0, this, [this, revision] { nextStep(revision); });
}

void CameraProbeService::cancel(quint64 operationId)
{
    if (!m_busy || !operationId || operationId != m_report.operationId || m_cancelRequested) return;
    m_cancelRequested = true;
    m_report.cancelled = true;
    if (m_stopReason.isEmpty())
        m_stopReason = tr("Camera probe cancelled. Remaining requests were not sent; already applied camera changes are not undone.");
    if (!m_command.isValid()) {
        finish(m_stopReason, true);
        return;
    }
    // The central waiter retains its token until ACK/timeout. Closing the
    // reservation forbids retries and permits an honest final attempt receipt.
    QPointer<CameraProbeService> guard(this);
    const quint64 revision = m_revision;
    if (m_commands) m_commands->releaseExactReservation(m_reservation);
    if (!guard || revision != m_revision || !m_busy) return;
    appendLog(tr("Cancelling remaining camera requests; waiting for the already-submitted command's acknowledgement or deadline."));
    if (guard && revision == m_revision) emit stateChanged();
}

void CameraProbeService::invalidateSource()
{
    m_prepared = Plan();
    if (m_finishing) return;
    if (m_busy) {
        if (!m_cancelRequested)
            m_stopReason = tr("Camera probe stopped because the selected modem, vehicle or camera changed. Remaining requests were not sent; already applied camera changes are not undone.");
        cancel(m_report.operationId);
    }
    else { ++m_revision; emit stateChanged(); }
}

void CameraProbeService::shutdown()
{
    if (m_shuttingDown) return;
    m_shuttingDown = true;
    m_prepared = Plan();
    if (m_busy) finish(tr("Camera probe stopped during shutdown; an outstanding camera command may already have taken effect."), true);
}

void CameraProbeService::finish(const QString &description, bool cancelled)
{
    if (!m_busy) return;
    QPointer<CameraProbeService> guard(this);
    const quint64 revision = ++m_revision;
    m_report.description = description;
    m_report.cancelled = m_report.cancelled || cancelled;
    if (m_command.isValid() && m_step < m_report.steps.size()) {
        m_report.steps[m_step].outcome = StepOutcome::OutcomeUncertain;
        m_report.steps[m_step].description = tr("Outstanding command detached; transmission or camera effect is not confirmed.");
    }
    m_lastReport = m_report;
    const Report completed = m_lastReport;
    m_finishing = true;
    m_busy = false;
    m_command = VehicleCommandService::ExactCommandToken();
    const auto reservation = m_reservation;
    m_reservation = VehicleCommandService::ExactReservationToken();
    if (m_commands && reservation.isValid()) m_commands->releaseExactReservation(reservation);
    if (!guard || revision != m_revision) return;
    appendLog(description);
    if (!guard || revision != m_revision) return;
    emit operationFinished(completed);
    if (guard && revision == m_revision) {
        m_finishing = false;
        emit stateChanged();
    }
}

void CameraProbeService::appendLog(const QString &message)
{
    m_history.append(message);
    while (m_history.size() > 128) m_history.removeFirst();
    emit logMessage(message);
}
