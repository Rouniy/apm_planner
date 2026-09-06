#include "GuidedNavigationService.h"

#include <QSharedData>
#include <QSignalBlocker>
#include <QThread>

#include <cmath>
#include <limits>
#include <utility>

namespace
{

bool reject(QString *error, const QString &description)
{
    if (error) *error = description;
    return false;
}

bool allowedFrame(MAV_FRAME frame)
{
    return frame == MAV_FRAME_GLOBAL
        || frame == MAV_FRAME_GLOBAL_RELATIVE_ALT
        || frame == MAV_FRAME_GLOBAL_TERRAIN_ALT;
}

QString purposeName(GuidedNavigationService::Purpose purpose)
{
    switch (purpose) {
    case GuidedNavigationService::Purpose::FlyToHere:
        return QStringLiteral("Fly to here");
    case GuidedNavigationService::Purpose::TerrainClick:
        return QStringLiteral("Terrain click");
    case GuidedNavigationService::Purpose::AltitudeUpdate:
        return QStringLiteral("Altitude update");
    case GuidedNavigationService::Purpose::Coordinates:
        return QStringLiteral("Coordinates");
    }
    return QStringLiteral("Guided navigation");
}

bool samePlan(const GuidedNavigationService::Plan &left,
              const GuidedNavigationService::Plan &right)
{
    return left.isValid() && right.isValid()
        && left.planId() == right.planId()
        && left.context().vehicle.sameInstance(right.context().vehicle)
        && left.context().target.generation
            == right.context().target.generation;
}

} // namespace

struct GuidedNavigationService::Plan::Data : public QSharedData
{
    quint64 id = 0;
    Purpose purpose = Purpose::FlyToHere;
    GuidedAltitudeStore::Context context;
    double latitude = 0;
    double longitude = 0;
    double altitudeM = 0;
    MAV_FRAME frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
    bool changeMode = false;
    QString description;
};

GuidedNavigationService::Plan::Plan() = default;
GuidedNavigationService::Plan::Plan(const Plan &) = default;
GuidedNavigationService::Plan &GuidedNavigationService::Plan::operator=(
    const Plan &) = default;
GuidedNavigationService::Plan::~Plan() = default;

bool GuidedNavigationService::Plan::isValid() const noexcept
{
    return d && d->id != 0 && d->context.target.isValid()
        && d->context.vehicle.isValid()
        && d->context.target.endpoint.sameIdentity(
            d->context.vehicle.endpoint);
}

quint64 GuidedNavigationService::Plan::planId() const noexcept
{
    return d ? d->id : 0;
}

GuidedNavigationService::Purpose
GuidedNavigationService::Plan::purpose() const noexcept
{
    return d ? d->purpose : Purpose::FlyToHere;
}

GuidedAltitudeStore::Context
GuidedNavigationService::Plan::context() const
{
    return d ? d->context : GuidedAltitudeStore::Context{};
}

VehicleTargetLease GuidedNavigationService::Plan::target() const
{
    return d ? d->context.target : VehicleTargetLease{};
}

SwarmVehicleInstanceLease GuidedNavigationService::Plan::vehicle() const
{
    return d ? d->context.vehicle : SwarmVehicleInstanceLease{};
}

double GuidedNavigationService::Plan::latitude() const noexcept
{
    return d ? d->latitude : 0;
}

double GuidedNavigationService::Plan::longitude() const noexcept
{
    return d ? d->longitude : 0;
}

double GuidedNavigationService::Plan::altitudeM() const noexcept
{
    return d ? d->altitudeM : 0;
}

MAV_FRAME GuidedNavigationService::Plan::frame() const noexcept
{
    return d ? d->frame : MAV_FRAME_GLOBAL_RELATIVE_ALT;
}

bool GuidedNavigationService::Plan::changeMode() const noexcept
{
    return d && d->changeMode;
}

QString GuidedNavigationService::Plan::description() const
{
    return d ? d->description : QString();
}

GuidedNavigationService::GuidedNavigationService(
    GuidedAltitudeStore *altitudeStore,
    GuidedTargetService *guidedTargetService,
    VehicleCommandService *commandService,
    QObject *parent)
    : QObject(parent)
    , m_altitudeStore(altitudeStore)
    , m_guidedTargetService(guidedTargetService)
    , m_commandService(commandService)
{
    qRegisterMetaType<Purpose>();
    qRegisterMetaType<Plan>();
    qRegisterMetaType<SubmitResult>();
    qRegisterMetaType<Outcome>();
    qRegisterMetaType<Report>();

    m_quarantinePoll.setSingleShot(true);
    m_quarantinePoll.setTimerType(Qt::PreciseTimer);
    connect(&m_quarantinePoll, &QTimer::timeout,
            this, &GuidedNavigationService::pollQuarantine);
    if (m_commandService) {
        connect(m_commandService,
                &VehicleCommandService::exactCommandFinished,
                this, &GuidedNavigationService::handleCommandFinished);
    }
    if (m_altitudeStore) {
        connect(m_altitudeStore, &QObject::destroyed, this, [this] {
            m_altitudeStore.clear();
            if (m_busy) cancel(m_report.operationId);
        });
    }
    if (m_guidedTargetService) {
        connect(m_guidedTargetService, &QObject::destroyed, this, [this] {
            m_guidedTargetService.clear();
            if (m_busy) cancel(m_report.operationId);
        });
    }
    if (m_commandService) {
        connect(m_commandService, &QObject::destroyed, this, [this] {
            m_commandService.clear();
            m_commandReservation = {};
            m_commandToken = {};
            if (m_busy) {
                finish(Outcome::OutcomeUncertain,
                       tr("The exact command service ended before the guided outcome was known."));
            }
        });
    }
}

GuidedNavigationService::~GuidedNavigationService()
{
    m_quarantinePoll.stop();
    if (m_uiOwnerDestroyedConnection) {
        disconnect(m_uiOwnerDestroyedConnection);
    }
    // This service is application-owned. Destruction is application teardown,
    // not a UI cancellation path; suppress callbacks while dropping local
    // reservations. VehicleCommandService retains any admitted ACK waiter.
    if (m_commandService && m_commandReservation.isValid()) {
        const QSignalBlocker blocker(m_commandService);
        m_commandService->releaseExactReservation(m_commandReservation);
    }
    if (m_guidedTargetService && m_guidedSession.isValid()) {
        const QSignalBlocker blocker(m_guidedTargetService);
        m_guidedTargetService->stop(m_guidedSession);
    }
}

bool GuidedNavigationService::busy() const noexcept
{
    return m_busy || m_finishing;
}

quint64 GuidedNavigationService::currentOperationId() const noexcept
{
    return m_busy ? m_report.operationId : 0;
}

QString GuidedNavigationService::status() const
{
    return m_status;
}

GuidedNavigationService::Plan GuidedNavigationService::activePlan() const
{
    return m_activePlan;
}

GuidedNavigationService::Report GuidedNavigationService::lastReport() const
{
    return m_lastReport;
}

bool GuidedNavigationService::prepare(
    const GuidedAltitudeStore::Context &context,
    double latitude, double longitude, double altitudeM,
    MAV_FRAME frame, bool changeMode, Purpose purpose,
    Plan *planOut, QString *error)
{
    const GuidedAltitudeStore::Context submittedContext = context;
    if (planOut) *planOut = Plan{};
    if (error) error->clear();
    if (!planOut) {
        return reject(error, tr("A guided navigation plan output is required."));
    }
    if (QThread::currentThread() != thread()) {
        return reject(error, tr("Guided navigation must be prepared on its owner thread."));
    }
    if (m_shuttingDown || busy() || m_apiInFlight) {
        return reject(error, tr("Another guided navigation operation is active."));
    }
    if (!m_altitudeStore || !m_guidedTargetService || !m_commandService) {
        return reject(error, tr("Guided navigation services are unavailable."));
    }
    if (m_preparedPlans.size() >= MaximumPreparedPlans) {
        return reject(error, tr("Too many guided navigation confirmations are pending."));
    }
    if (!std::isfinite(latitude) || !std::isfinite(longitude)
        || latitude < -90.0 || latitude > 90.0
        || longitude < -180.0 || longitude > 180.0
        || (latitude == 0.0 && longitude == 0.0)) {
        return reject(error, tr("The guided latitude or longitude is outside its finite geographic range."));
    }
    if (!std::isfinite(altitudeM)
        || std::abs(altitudeM) > double(std::numeric_limits<float>::max())) {
        return reject(error, tr("The guided altitude must be a finite float value in metres."));
    }
    const float wireAltitude = float(altitudeM);
    if (altitudeM != 0.0 && wireAltitude == 0.0F) {
        return reject(error, tr("The guided altitude is too small for the MAVLink wire value."));
    }
    if (wireAltitude == 0.0F) {
        // MP10 setGuidedModeWP suppresses a zero-height request. Keep zero as
        // editable/unset intent, but do not turn its silent no-op into movement.
        return reject(error, tr("Set a non-zero guided altitude before sending a target."));
    }
    if (!allowedFrame(frame)) {
        return reject(error, tr("The guided altitude frame is unsupported."));
    }
    const int purposeValue = int(purpose);
    if (purposeValue < int(Purpose::FlyToHere)
        || purposeValue > int(Purpose::Coordinates)) {
        return reject(error, tr("The guided navigation purpose is invalid."));
    }
    if (purpose == Purpose::TerrainClick
        && (!submittedContext.altitudeSet
            || std::abs(submittedContext.altitudeM) < 0.01F
            || frame != MAV_FRAME_GLOBAL_RELATIVE_ALT
            || changeMode
            || altitudeM != double(submittedContext.altitudeM))) {
        return reject(error, tr("Terrain click requires the unchanged explicit relative guided altitude and no mode change."));
    }

    m_apiInFlight = true;
    QPointer<GuidedNavigationService> guard(this);
    const QPointer<GuidedAltitudeStore> store = m_altitudeStore;
    QString validationError;
    const bool contextValid = store->validate(
        submittedContext, &validationError);
    if (!guard) return false;
    m_apiInFlight = false;
    if (!contextValid || store != m_altitudeStore || m_shuttingDown
        || busy()) {
        return reject(error, validationError.isEmpty()
            ? tr("The guided altitude context changed during preparation.")
            : validationError);
    }

    const quint64 id = nextPlanId();
    if (id == 0) {
        return reject(error, tr("Guided plan identifier space is exhausted."));
    }
    Plan plan;
    plan.d = new Plan::Data;
    plan.d->id = id;
    plan.d->purpose = purpose;
    plan.d->context = submittedContext;
    plan.d->latitude = latitude;
    plan.d->longitude = longitude;
    plan.d->altitudeM = altitudeM;
    plan.d->frame = frame;
    plan.d->changeMode = changeMode;
    plan.d->description = tr(
        "%1: link %2, system/component %3/%4; %5, %6, %7 m; frame %8; mode change %9")
        .arg(purposeName(purpose))
        .arg(submittedContext.target.endpoint.linkId)
        .arg(submittedContext.target.endpoint.systemId)
        .arg(submittedContext.target.endpoint.componentId)
        .arg(latitude, 0, 'g', 12)
        .arg(longitude, 0, 'g', 12)
        .arg(altitudeM, 0, 'g', 9)
        .arg(int(frame))
        .arg(changeMode ? tr("requested") : tr("not requested"));
    m_preparedPlans.append(plan);
    *planOut = plan;
    m_status = tr("Guided navigation plan prepared; explicit confirmation is required.");
    emit stateChanged();
    return true;
}

bool GuidedNavigationService::planMatchesPrepared(
    const Plan &plan) const noexcept
{
    for (const Plan &prepared : m_preparedPlans) {
        if (samePlan(plan, prepared)
            && plan.d.constData() == prepared.d.constData()) {
            return true;
        }
    }
    return false;
}

bool GuidedNavigationService::discardPlan(const Plan &plan)
{
    if (QThread::currentThread() != thread() || !plan.isValid()) {
        return false;
    }
    for (int index = 0; index < m_preparedPlans.size(); ++index) {
        const Plan &prepared = m_preparedPlans.at(index);
        if (!samePlan(plan, prepared)
            || plan.d.constData() != prepared.d.constData()) {
            continue;
        }
        m_preparedPlans.removeAt(index);
        if (!busy()) {
            m_status = tr("Guided navigation plan discarded.");
        }
        emit stateChanged();
        return true;
    }
    return false;
}

bool GuidedNavigationService::operationIsCurrent(
    quint64 operationId, const Plan &plan) const noexcept
{
    return m_busy && operationId != 0
        && m_report.operationId == operationId
        && samePlan(plan, m_activePlan)
        && plan.d.constData() == m_activePlan.d.constData();
}

bool GuidedNavigationService::validatePlan(
    const Plan &plan, bool requirePrepared, QString *error) const
{
    const Plan submitted = plan;
    if (!submitted.isValid()) {
        return reject(error, tr("The guided navigation plan is invalid."));
    }
    if (requirePrepared && !planMatchesPrepared(submitted)) {
        return reject(error, tr("The guided navigation plan is stale or was already consumed."));
    }
    if (!m_altitudeStore || !m_guidedTargetService || !m_commandService) {
        return reject(error, tr("Guided navigation services are unavailable."));
    }
    if (!std::isfinite(submitted.latitude())
        || !std::isfinite(submitted.longitude())
        || !std::isfinite(submitted.altitudeM())
        || submitted.latitude() < -90.0 || submitted.latitude() > 90.0
        || submitted.longitude() < -180.0
        || submitted.longitude() > 180.0
        || !allowedFrame(submitted.frame())) {
        return reject(error, tr("The immutable guided navigation payload is invalid."));
    }
    const QPointer<const GuidedNavigationService> guard(this);
    const QPointer<GuidedAltitudeStore> store = m_altitudeStore;
    const bool valid = store->validate(submitted.context(), error);
    if (!guard) return false;
    if (!valid || store != m_altitudeStore) return false;
    if (requirePrepared && !planMatchesPrepared(submitted)) {
        return reject(error, tr("The guided navigation plan changed during validation."));
    }
    return true;
}

bool GuidedNavigationService::validate(
    const Plan &plan, QString *error) const
{
    if (error) error->clear();
    if (QThread::currentThread() != thread()) {
        return reject(error, tr("Guided navigation must be validated on its owner thread."));
    }
    return !m_shuttingDown && !busy()
        && validatePlan(plan, true, error);
}

GuidedNavigationService::SubmitResult GuidedNavigationService::execute(
    QObject *uiOwner, const Plan &plan, quint64 *operationIdOut,
    QString *error)
{
    const Plan submitted = plan;
    if (operationIdOut) *operationIdOut = 0;
    if (error) error->clear();
    if (QThread::currentThread() != thread()
        || !uiOwner || uiOwner == this || uiOwner->thread() != thread()) {
        if (error) *error = tr("A live UI owner on the service thread is required.");
        return SubmitResult::InvalidOwner;
    }
    if (m_shuttingDown || busy() || m_apiInFlight) {
        if (error) *error = tr("Another guided navigation operation is active.");
        return SubmitResult::Busy;
    }
    if (!planMatchesPrepared(submitted)) {
        if (error) *error = tr("The guided navigation plan is stale or was already consumed.");
        return SubmitResult::InvalidPlan;
    }
    if (!m_altitudeStore || !m_guidedTargetService || !m_commandService) {
        if (error) *error = tr("Guided navigation services are unavailable.");
        return SubmitResult::Unavailable;
    }
    const quint64 operationId = nextOperationId();
    if (operationId == 0) {
        if (error) *error = tr("Guided operation identifier space is exhausted.");
        return SubmitResult::Unavailable;
    }

    m_busy = true;
    m_cancelRequested = false;
    m_processingCommandReport = false;
    m_waitingForQuarantine = false;
    m_deferredReports.clear();
    m_commandReservation = {};
    m_commandToken = {};
    m_guidedSession = {};
    m_activePlan = submitted;
    for (int index = 0; index < m_preparedPlans.size(); ++index) {
        const Plan &prepared = m_preparedPlans.at(index);
        if (samePlan(submitted, prepared)
            && submitted.d.constData() == prepared.d.constData()) {
            m_preparedPlans.removeAt(index);
            break;
        }
    }
    m_report = {};
    m_report.operationId = operationId;
    m_report.plan = submitted;
    m_uiOwner = uiOwner;
    if (m_uiOwnerDestroyedConnection) disconnect(m_uiOwnerDestroyedConnection);
    m_uiOwnerDestroyedConnection = connect(
        uiOwner, &QObject::destroyed, this,
        [this, operationId]() { handleUiOwnerDestroyed(operationId); });
    if (operationIdOut) *operationIdOut = operationId;
    m_status = tr("Validating the consented guided target.");

    m_apiInFlight = true;
    QPointer<GuidedNavigationService> guard(this);
    emit stateChanged();
    if (!guard) return SubmitResult::Unavailable;
    if (!operationIsCurrent(operationId, submitted)) {
        m_apiInFlight = false;
        return SubmitResult::Started;
    }

    QString validationError;
    const bool valid = validatePlan(submitted, false, &validationError);
    if (!guard) return SubmitResult::Unavailable;
    if (!operationIsCurrent(operationId, submitted)) {
        m_apiInFlight = false;
        return SubmitResult::Started;
    }
    if (!valid || m_cancelRequested) {
        m_apiInFlight = false;
        finish(m_cancelRequested ? Outcome::Cancelled : Outcome::Rejected,
               m_cancelRequested ? tr("Guided navigation was cancelled before transmission.")
                                 : (validationError.isEmpty()
                                        ? tr("The guided target changed before transmission.")
                                        : validationError));
        return SubmitResult::Started;
    }

    GuidedTargetService::SessionToken guidedSession;
    const auto guidedResult = m_guidedTargetService->reserve(
        this, submitted.target(), &guidedSession);
    if (!guard) return SubmitResult::Unavailable;
    if (!operationIsCurrent(operationId, submitted)) {
        if (guidedSession.isValid() && m_guidedTargetService) {
            m_guidedTargetService->stop(guidedSession);
            if (!guard) return SubmitResult::Unavailable;
        }
        m_apiInFlight = false;
        return SubmitResult::Started;
    }
    if (guidedResult != GuidedTargetService::RequestResult::Started
        || !guidedSession.isValid()) {
        m_apiInFlight = false;
        finish(m_cancelRequested ? Outcome::Cancelled : Outcome::Rejected,
               m_cancelRequested
                   ? tr("Guided navigation was cancelled before transmission.")
                   : GuidedTargetService::resultDescription(guidedResult));
        return SubmitResult::Started;
    }
    m_guidedSession = guidedSession;
    if (m_cancelRequested) {
        m_apiInFlight = false;
        finish(Outcome::Cancelled,
               tr("Guided navigation was cancelled before transmission."));
        return SubmitResult::Started;
    }

    VehicleCommandService::ExactReservationToken reservation;
    const auto reserveResult = m_commandService->reserveSingleVehicleEndpoint(
        this, submitted.target(), submitted.vehicle(), &reservation,
        &validationError);
    if (!guard) return SubmitResult::Unavailable;
    if (!operationIsCurrent(operationId, submitted)) {
        if (reservation.isValid() && m_commandService) {
            m_commandService->releaseExactReservation(reservation);
            if (!guard) return SubmitResult::Unavailable;
        }
        m_apiInFlight = false;
        return SubmitResult::Started;
    }
    if (reserveResult != VehicleCommandService::ExactReservationResult::Reserved
        || !reservation.isValid()) {
        m_apiInFlight = false;
        finish(m_cancelRequested ? Outcome::Cancelled : Outcome::Rejected,
               m_cancelRequested
                   ? tr("Guided navigation was cancelled before transmission.")
                   : (validationError.isEmpty()
                          ? tr("The exact guided command route is unavailable.")
                          : validationError));
        return SubmitResult::Started;
    }
    m_commandReservation = reservation;
    if (m_cancelRequested) {
        m_commandService->releaseExactReservation(m_commandReservation);
        if (!guard) return SubmitResult::Unavailable;
        m_commandReservation = {};
        m_apiInFlight = false;
        finish(Outcome::Cancelled,
               tr("Guided navigation was cancelled before transmission."));
        return SubmitResult::Started;
    }

    VehicleCommandService::ExactCommandIntRequest request;
    request.command = MAV_CMD_DO_REPOSITION;
    request.frame = submitted.frame();
    request.params = {-1.0F, submitted.changeMode() ? 1.0F : 0.0F,
                      0.0F, std::numeric_limits<float>::quiet_NaN()};
    request.x = qint32(submitted.latitude() * 1.0e7);
    request.y = qint32(submitted.longitude() * 1.0e7);
    request.z = float(submitted.altitudeM());
    request.current = 0;
    request.autocontinue = 0;
    request.acknowledgementTimeoutMs = m_acknowledgementTimeoutMs;
    request.maximumLifetimeMs = m_maximumLifetimeMs;
    request.maximumRetries = m_maximumRetries;
    request.validateBeforeWrite =
        [guard, operationId, submitted](QString *gateError) {
            return guard && guard->finalWriteGate(
                operationId, submitted, gateError);
        };

    VehicleCommandService::ExactCommandToken commandToken;
    m_submitting = true;
    const auto submitResult = m_commandService->submitExactCommandInt(
        m_commandReservation, submitted.vehicle(), request,
        &commandToken, &validationError);
    if (!guard) return SubmitResult::Unavailable;
    m_submitting = false;
    m_apiInFlight = false;
    if (!operationIsCurrent(operationId, submitted)) {
        return SubmitResult::Started;
    }
    if (commandToken.isValid()) m_commandToken = commandToken;

    const QList<VehicleCommandService::ExactCommandReport> deferred =
        std::exchange(
            m_deferredReports,
            QList<VehicleCommandService::ExactCommandReport>{});
    for (const auto &commandReport : deferred) {
        if (!operationIsCurrent(operationId, submitted)) break;
        if (m_commandToken.isValid()
            && commandReport.token.transactionId
                == m_commandToken.transactionId) {
            processCommandReport(commandReport);
            if (!guard) return SubmitResult::Unavailable;
        }
    }
    if (!operationIsCurrent(operationId, submitted)) {
        return SubmitResult::Started;
    }
    // A synchronous terminal report may have moved this operation into the
    // central late-ACK drain while submitExactCommandInt still returns its
    // transport result. Do not reinterpret that already-recorded report or
    // release the guided lane early.
    if (m_waitingForQuarantine) {
        return SubmitResult::Started;
    }
    if (submitResult != VehicleCommandService::ExactSubmitResult::Started) {
        const bool uncertain = submitResult
            == VehicleCommandService::ExactSubmitResult::
                TransportOutcomeUncertain;
        finish(m_cancelRequested ? Outcome::Cancelled
                                 : uncertain ? Outcome::OutcomeUncertain
                                             : Outcome::Rejected,
               m_cancelRequested
                   ? tr("Guided navigation was cancelled before transmission.")
                   : (validationError.isEmpty()
                          ? tr("The exact guided command could not be submitted.")
                          : validationError));
        return SubmitResult::Started;
    }

    m_status = tr("Waiting for the exact vehicle acknowledgement.");
    emit stateChanged();
    return SubmitResult::Started;
}

bool GuidedNavigationService::finalWriteGate(
    quint64 operationId, const Plan &plan, QString *error)
{
    const Plan submitted = plan;
    if (!operationIsCurrent(operationId, submitted)
        || m_cancelRequested || m_shuttingDown) {
        return reject(error, tr("The guided operation was cancelled before frame transmission."));
    }
    QPointer<GuidedNavigationService> guard(this);
    const QPointer<GuidedAltitudeStore> store = m_altitudeStore;
    if (!store) {
        return reject(error, tr("The guided altitude store is unavailable."));
    }
    const bool valid = store->validate(submitted.context(), error);
    if (!guard) return false;
    if (!valid || store != m_altitudeStore
        || !operationIsCurrent(operationId, submitted)
        || m_cancelRequested || m_shuttingDown) {
        return reject(error, error && !error->isEmpty() ? *error
            : tr("The guided context changed at the final write boundary."));
    }
    return true;
}

void GuidedNavigationService::handleCommandFinished(
    const VehicleCommandService::ExactCommandReport &report)
{
    if (!m_busy || report.token.command != MAV_CMD_DO_REPOSITION
        || !m_activePlan.isValid()
        || report.token.reservationId != m_commandReservation.reservationId
        || !report.token.lease.sameInstance(m_activePlan.vehicle())) {
        return;
    }
    if (m_submitting) {
        if (m_deferredReports.isEmpty()) m_deferredReports.append(report);
        return;
    }
    if (!m_commandToken.isValid()
        || report.token.transactionId != m_commandToken.transactionId) {
        return;
    }
    processCommandReport(report);
}

void GuidedNavigationService::processCommandReport(
    const VehicleCommandService::ExactCommandReport &commandReport)
{
    if (!m_busy || !m_activePlan.isValid()) return;
    const quint64 operationId = m_report.operationId;
    const Plan plan = m_activePlan;
    m_processingCommandReport = true;
    m_report.mavResult = commandReport.mavResult;
    m_report.transmissionAttempts = commandReport.transmissionAttempts;
    m_report.frameAttempted = commandReport.frameAttempted;
    m_report.cancellationRequested = m_cancelRequested;

    Outcome outcome = Outcome::Rejected;
    QString description = commandReport.description;
    switch (commandReport.terminalResult) {
    case VehicleCommandService::ExactTerminalResult::AcknowledgedAccepted:
        outcome = Outcome::Accepted;
        break;
    case VehicleCommandService::ExactTerminalResult::AcknowledgedRejected:
        outcome = Outcome::Rejected;
        break;
    case VehicleCommandService::ExactTerminalResult::RejectedBeforeTransmission:
        outcome = m_cancelRequested ? Outcome::Cancelled : Outcome::Rejected;
        break;
    case VehicleCommandService::ExactTerminalResult::TimedOutOutcomeUncertain:
    case VehicleCommandService::ExactTerminalResult::TransportOutcomeUncertain:
    case VehicleCommandService::ExactTerminalResult::LeaseRetiredOutcomeUncertain:
    case VehicleCommandService::ExactTerminalResult::LinkForgottenOutcomeUncertain:
        outcome = Outcome::OutcomeUncertain;
        break;
    }

    QPointer<GuidedNavigationService> guard(this);
    if (outcome == Outcome::Accepted && m_altitudeStore) {
        GuidedAltitudeStore::Context recorded;
        const bool targetRecorded = m_altitudeStore->recordTarget(
            plan.context(), plan.latitude(), plan.longitude(),
            plan.altitudeM(), plan.frame(), &recorded);
        if (!guard) return;
        m_report.targetRecorded = targetRecorded;
        if (!m_report.targetRecorded) {
            description += tr(" The vehicle accepted the target, but the local guided context had changed and was not overwritten.");
        }
    }
    m_commandToken = {};

    if (m_commandService && m_commandReservation.isValid()) {
        const auto reservation = m_commandReservation;
        m_commandReservation = {};
        m_commandService->releaseExactReservation(reservation);
        if (!guard) return;
    }
    if (!operationIsCurrent(operationId, plan)) return;

    m_pendingOutcome = outcome;
    m_pendingDescription = description.isEmpty()
        ? tr("The guided command reached a terminal state.") : description;
    const bool quarantined = m_commandService
        && m_commandService->isExactCommandQuarantined(
            plan.vehicle(), MAV_CMD_DO_REPOSITION);
    if (quarantined) {
        m_waitingForQuarantine = true;
        m_processingCommandReport = false;
        m_status = tr("The command outcome is final locally; retaining the guided lane until the shared late-ACK quarantine drains.");
        emit stateChanged();
        if (guard && operationIsCurrent(operationId, plan)) {
            m_quarantinePoll.start(50);
        }
        return;
    }
    m_processingCommandReport = false;
    finish(outcome, m_pendingDescription);
}

void GuidedNavigationService::pollQuarantine()
{
    if (!m_busy || !m_waitingForQuarantine || !m_activePlan.isValid()) {
        return;
    }
    const Plan plan = m_activePlan;
    const quint64 operationId = m_report.operationId;
    if (m_commandService
        && m_commandService->isExactCommandQuarantined(
            plan.vehicle(), MAV_CMD_DO_REPOSITION)) {
        if (operationIsCurrent(operationId, plan)) {
            m_quarantinePoll.start(50);
        }
        return;
    }
    m_waitingForQuarantine = false;
    finish(m_pendingOutcome, m_pendingDescription);
}

bool GuidedNavigationService::cancel(quint64 operationId)
{
    if (!m_busy || operationId == 0
        || operationId != m_report.operationId) {
        return false;
    }
    if (m_cancelRequested) return true;
    m_cancelRequested = true;
    m_report.cancellationRequested = true;
    if (m_uiOwnerDestroyedConnection) {
        disconnect(m_uiOwnerDestroyedConnection);
        m_uiOwnerDestroyedConnection = {};
    }
    m_uiOwner.clear();
    m_status = tr("Cancellation requested; draining any exact command outcome and late-ACK quarantine.");

    QPointer<GuidedNavigationService> guard(this);
    if (m_commandService && m_commandReservation.isValid()) {
        m_commandService->releaseExactReservation(m_commandReservation);
        if (!guard) return true;
    }
    emit stateChanged();
    if (!guard || !m_busy || operationId != m_report.operationId) return true;
    if (m_submitting || m_processingCommandReport
        || m_commandToken.isValid()
        || m_waitingForQuarantine) {
        return true;
    }
    finish(Outcome::Cancelled,
           tr("Guided navigation was cancelled before transmission."));
    return true;
}

void GuidedNavigationService::shutdown()
{
    m_shuttingDown = true;
    m_preparedPlans.clear();
    if (m_busy) {
        cancel(m_report.operationId);
    } else {
        m_status = tr("Guided navigation service shut down.");
        emit stateChanged();
    }
}

void GuidedNavigationService::setTimeoutsForTesting(
    int acknowledgementMs, int maximumLifetimeMs, int maximumRetries)
{
    if (busy()) return;
    m_acknowledgementTimeoutMs = qBound(1, acknowledgementMs,
        DefaultMaximumLifetimeMs);
    m_maximumLifetimeMs = qBound(
        m_acknowledgementTimeoutMs, maximumLifetimeMs,
        DefaultMaximumLifetimeMs);
    m_maximumRetries = qBound(0, maximumRetries, 3);
}

void GuidedNavigationService::releaseLanes()
{
    m_quarantinePoll.stop();
    QPointer<GuidedNavigationService> guard(this);
    if (m_commandService && m_commandReservation.isValid()) {
        const auto reservation = m_commandReservation;
        m_commandReservation = {};
        m_commandService->releaseExactReservation(reservation);
        if (!guard) return;
    }
    if (m_guidedTargetService && m_guidedSession.isValid()) {
        const auto session = m_guidedSession;
        m_guidedSession = {};
        m_guidedTargetService->stop(session);
    }
}

void GuidedNavigationService::finish(
    Outcome outcome, const QString &description)
{
    if (!m_busy || m_finishing) return;
    const quint64 operationId = m_report.operationId;
    const Plan plan = m_activePlan;
    m_finishing = true;
    m_report.outcome = outcome;
    m_report.cancellationRequested = m_cancelRequested;
    m_report.description = description;

    QPointer<GuidedNavigationService> guard(this);
    releaseLanes();
    if (!guard) return;
    if (!m_busy || m_report.operationId != operationId
        || !samePlan(m_activePlan, plan)) {
        return;
    }
    if (m_uiOwnerDestroyedConnection) {
        disconnect(m_uiOwnerDestroyedConnection);
        m_uiOwnerDestroyedConnection = {};
    }
    m_uiOwner.clear();
    m_waitingForQuarantine = false;
    m_processingCommandReport = false;
    m_deferredReports.clear();
    m_activePlan = {};
    m_lastReport = m_report;
    m_status = description;
    m_busy = false;

    emit stateChanged();
    if (!guard) return;
    emit operationFinished(m_lastReport);
    if (!guard) return;
    m_finishing = false;
    emit stateChanged();
}

void GuidedNavigationService::handleUiOwnerDestroyed(
    quint64 operationId)
{
    cancel(operationId);
}

quint64 GuidedNavigationService::nextPlanId()
{
    if (m_nextPlanId == std::numeric_limits<quint64>::max()) return 0;
    return ++m_nextPlanId;
}

quint64 GuidedNavigationService::nextOperationId()
{
    if (m_nextOperationId == std::numeric_limits<quint64>::max()) return 0;
    return ++m_nextOperationId;
}
