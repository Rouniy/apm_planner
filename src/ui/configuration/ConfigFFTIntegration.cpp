#include "ConfigFFTIntegration.h"

#include "ConfigFFTView.h"
#include "core/parameters/ParameterStore.h"

#include <QTimer>

#include <utility>

namespace {

bool samePinnedTarget(const VehicleTargetLease &target,
                      VehicleTargetManager *targets)
{
    return target.isValid() && targets
        && targets->isCurrentTarget(
            target.endpoint.linkId, target.endpoint.systemId,
            target.endpoint.componentId, target.generation);
}

bool writeSucceeded(ParameterService::ExactTerminalResult result)
{
    return result == ParameterService::ExactTerminalResult::WriteSucceeded
        || result == ParameterService::ExactTerminalResult::WriteSkipped;
}

} // namespace

ConfigFFTIntegration::ConfigFFTIntegration(
    ConfigFFTView *view,
    ParameterService *parameters,
    VehicleTargetManager *targets,
    ExactLeaseProvider exactLeaseProvider,
    QObject *parent)
    : QObject(parent ? parent : view)
    , m_view(view)
    , m_parameters(parameters)
    , m_targets(targets)
    , m_exactLeaseProvider(std::move(exactLeaseProvider))
{
    if (m_targets) {
        m_target = m_targets->acquireTarget();
    }
    if (m_target.isValid() && m_exactLeaseProvider) {
        m_vehicle = m_exactLeaseProvider(m_target);
    }
    if (!m_target.isValid() || !m_vehicle.isValid()
        || !m_vehicle.endpoint.sameIdentity(m_target.endpoint)) {
        m_retired = true;
    }

    if (m_view) {
        connect(m_view, &ConfigFFTView::refreshRequested,
                this, &ConfigFFTIntegration::beginRefresh);
        connect(m_view, &ConfigFFTView::writeRequested,
                this, &ConfigFFTIntegration::beginWrite);
    }
    if (m_parameters) {
        connect(m_parameters, &ParameterService::exactOperationFinished,
                this, &ConfigFFTIntegration::receiveReport);
        connect(m_parameters, &QObject::destroyed, this, [this]() {
            const QPointer<ConfigFFTIntegration> guard(this);
            m_parameters = nullptr;
            const Activity activity = m_activity;
            const quint64 requestId = m_writeRequestId;
            const qulonglong batchId = m_writeBatchId;
            const QString name = m_writeName;
            m_reservation = {};
            m_operation = {};
            m_activity = Activity::None;
            m_retired = true;
            syncParameterContext();
            if (!guard || !m_view) {
                return;
            }
            if (activity == Activity::Refresh) {
                m_view->refreshFailed(tr(
                    "The exact parameter service stopped."));
            } else if (activity == Activity::Write) {
                if (batchId != 0) {
                    m_view->parameterWriteFailed(
                        batchId, m_target.endpoint.componentId, name,
                        tr("The exact parameter service stopped."));
                } else {
                    m_view->parameterWriteSubmissionFailed(
                        requestId,
                        tr("The exact parameter service stopped."));
                }
            }
        });
        if (m_parameters->store()) {
            ParameterStore *const store = m_parameters->store();
            connect(store, &ParameterStore::endpointStateChanged,
                    this,
                    [this](int linkId, int systemId, int componentId,
                           ParameterLoadState state) {
                if (m_activity == Activity::None
                    && state == ParameterLoadState::Complete
                    && linkId == m_target.endpoint.linkId
                    && systemId == m_target.endpoint.systemId
                    && componentId == m_target.endpoint.componentId) {
                    publishInitialSnapshot();
                }
            });
            connect(store, &ParameterStore::endpointParameterChanged,
                    this,
                    [this](int linkId, int systemId, int componentId,
                           const QString &) {
                if (m_activity == Activity::None
                    && linkId == m_target.endpoint.linkId
                    && systemId == m_target.endpoint.systemId
                    && componentId == m_target.endpoint.componentId) {
                    publishInitialSnapshot();
                }
            });
        }
    }
    if (m_targets) {
        const auto targetChanged = [this]() {
            if (!samePinnedTarget(m_target, m_targets)) {
                retirePinnedTarget(tr(
                    "The selected vehicle or physical link changed."));
            } else {
                syncParameterContext();
            }
        };
        connect(m_targets, &VehicleTargetManager::currentTargetChanged,
                this, targetChanged);
        connect(m_targets, &VehicleTargetManager::endpointUpdated,
                this, [targetChanged](int, int, int) { targetChanged(); });
        connect(m_targets, &QObject::destroyed, this, [this]() {
            m_targets = nullptr;
            retirePinnedTarget(tr("The vehicle target service stopped."));
        });
    }

    m_freshnessTimer = new QTimer(this);
    m_freshnessTimer->setObjectName(
        QStringLiteral("configFftExactTargetFreshnessTimer"));
    m_freshnessTimer->setInterval(250);
    connect(m_freshnessTimer, &QTimer::timeout, this, [this]() {
        if (!m_retired) {
            const QPointer<ConfigFFTIntegration> guard(this);
            if (!exactLeaseStillCurrent()) {
                if (guard) {
                    retirePinnedTarget(tr(
                        "The pinned vehicle instance or route retired."));
                }
                return;
            }
            if (!guard) {
                return;
            }
        }
        syncParameterContext();
    });
    m_freshnessTimer->start();

    syncParameterContext();
    publishInitialSnapshot();
}

ConfigFFTIntegration::~ConfigFFTIntegration()
{
    m_destroying = true;
    if (m_parameters && m_operation.isValid()
        && m_reservation.isValid()) {
        disconnect(m_parameters, nullptr, this, nullptr);
        m_parameters->cancelExactOperation(
            m_reservation, m_operation,
            QStringLiteral("The FFT page was closed."));
    }
    if (m_parameters && m_reservation.isValid()) {
        m_parameters->releaseExactReservation(m_reservation);
    }
}

QStringList ConfigFFTIntegration::parameterNames()
{
    return {
        QStringLiteral("INS_LOG_BAT_CNT"),
        QStringLiteral("INS_LOG_BAT_MASK"),
        QStringLiteral("LOG_BITMASK")};
}

bool ConfigFFTIntegration::parameterTargetUsable() const
{
    return !m_retired && m_view && m_parameters && m_targets
        && m_vehicle.isValid()
        && samePinnedTarget(m_target, m_targets);
}

bool ConfigFFTIntegration::exactLeaseStillCurrent()
{
    if (!parameterTargetUsable() || !m_exactLeaseProvider) {
        return false;
    }
    const ExactLeaseProvider provider = m_exactLeaseProvider;
    const VehicleTargetLease target = m_target;
    const SwarmVehicleInstanceLease pinned = m_vehicle;
    const QPointer<ConfigFFTIntegration> guard(this);
    const SwarmVehicleInstanceLease current = provider(target);
    return guard && current.isValid() && current.sameInstance(pinned);
}

bool ConfigFFTIntegration::beginReservation(
    Activity activity, QString *error)
{
    if (error) {
        error->clear();
    }
    if (m_activity != Activity::None || m_submitting) {
        if (error) {
            *error = tr("Another FFT parameter operation is active.");
        }
        return false;
    }
    const QPointer<ConfigFFTIntegration> guard(this);
    if (!exactLeaseStillCurrent()) {
        if (guard) {
            retirePinnedTarget(tr(
                "The pinned vehicle instance is no longer current."));
            if (error) {
                *error = tr(
                    "The pinned vehicle instance is no longer current.");
            }
        }
        return false;
    }
    if (!guard || !m_parameters) {
        return false;
    }

    m_activity = activity;
    ParameterService::ExactReservationToken reservation;
    QString detail;
    const ParameterService::ExactReservationResult result =
        m_parameters->reserveSingleVehicleEndpoint(
            this, m_target, m_vehicle, &reservation, &detail);
    if (!guard) {
        return false;
    }
    if (result != ParameterService::ExactReservationResult::Reserved) {
        m_activity = Activity::None;
        if (error) {
            *error = reservationError(result, detail);
        }
        return false;
    }
    m_reservation = reservation;
    if (m_retired || !exactLeaseStillCurrent()) {
        if (guard) {
            m_activity = Activity::None;
            releaseReservation();
            if (!guard) {
                return false;
            }
            if (error) {
                *error = tr(
                    "The pinned vehicle changed while reserving parameters.");
            }
        }
        return false;
    }
    return bool(guard);
}

void ConfigFFTIntegration::releaseReservation()
{
    const ParameterService::ExactReservationToken reservation =
        m_reservation;
    m_reservation = {};
    if (m_parameters && reservation.isValid()) {
        m_parameters->releaseExactReservation(reservation);
    }
}

void ConfigFFTIntegration::syncParameterContext()
{
    if (!m_view) {
        return;
    }
    const bool connected = parameterTargetUsable();
    const bool fresh = connected && m_targets->hasFreshHeartbeat(
        m_target, 3000);
    const bool armed = fresh && m_targets->heartbeatArmed(m_target);
    m_view->setParameterContext(connected, fresh, armed);
}

void ConfigFFTIntegration::publishInitialSnapshot()
{
    if (!m_view || !m_parameters || !m_parameters->store()
        || !m_target.isValid()) {
        return;
    }
    const ParameterSnapshot snapshot =
        m_parameters->store()->snapshot(m_target.endpoint);
    QList<ConfigFriendlyParameterValue> values;
    if (snapshot.isComplete()
        && snapshot.endpoint().sameIdentity(m_target.endpoint)) {
        const QStringList wanted = parameterNames();
        for (const ParameterRecord &record : snapshot.records()) {
            if (record.key.componentId
                    != m_target.endpoint.componentId
                || !wanted.contains(record.key.name)) {
                continue;
            }
            values.append({record.key.componentId,
                           record.key.name, record.value});
            m_types.insert(record.key.name, record.type);
        }
    }
    m_view->setParameterSnapshot(
        values, m_target.isValid() ? m_target.endpoint.componentId : 1,
        snapshot.isComplete()
            && snapshot.endpoint().sameIdentity(m_target.endpoint));
}

void ConfigFFTIntegration::retirePinnedTarget(const QString &reason)
{
    if (m_retired && m_activity == Activity::None) {
        syncParameterContext();
        return;
    }
    m_retired = true;
    const QPointer<ConfigFFTIntegration> guard(this);
    syncParameterContext();
    if (!guard || m_destroying || m_activity == Activity::None) {
        return;
    }
    if (m_submitting) {
        return;
    }
    if (m_parameters && m_reservation.isValid()
        && m_operation.isValid()
        && m_parameters->cancelExactOperation(
            m_reservation, m_operation, reason)) {
        return;
    }
    if (m_activity == Activity::Refresh) {
        finishRefreshFailure(reason);
    } else {
        finishWriteFailure(reason, m_writeBatchId != 0);
    }
}

void ConfigFFTIntegration::beginRefresh(int componentId)
{
    const QPointer<ConfigFFTIntegration> guard(this);
    if (componentId != m_target.endpoint.componentId) {
        if (m_view) {
            m_view->refreshFailed(tr(
                "The FFT page is pinned to another component."));
        }
        return;
    }
    QString error;
    if (!beginReservation(Activity::Refresh, &error)) {
        if (guard && m_view) {
            m_view->refreshFailed(error.isEmpty()
                ? tr("The exact parameter refresh was rejected.") : error);
        }
        return;
    }
    if (!guard) {
        return;
    }
    m_refreshNames = parameterNames();
    m_refreshIndex = 0;
    m_refreshValues.clear();
    submitNextRead();
}

void ConfigFFTIntegration::submitNextRead()
{
    if (m_activity != Activity::Refresh || !m_parameters
        || !m_reservation.isValid()
        || m_refreshIndex < 0
        || m_refreshIndex >= m_refreshNames.size()) {
        finishRefreshFailure(tr(
            "The exact parameter refresh lost its reservation."));
        return;
    }

    ParameterService::ExactReadRequest request;
    request.name = m_refreshNames.at(m_refreshIndex);
    ParameterService::ExactOperationToken operation;
    QString detail;
    m_deferredReports.clear();
    m_submitting = true;
    const QPointer<ConfigFFTIntegration> guard(this);
    const ParameterService::ExactSubmitResult result =
        m_parameters->submitExactRead(
            m_reservation, m_vehicle, request, &operation, &detail);
    if (!guard) {
        return;
    }
    m_submitting = false;
    m_operation = operation;
    if (m_retired && m_operation.isValid()) {
        m_parameters->cancelExactOperation(
            m_reservation, m_operation,
            tr("The selected vehicle changed during parameter refresh."));
    }
    if (!guard) {
        return;
    }

    const QVector<ParameterService::ExactOperationReport> reports =
        std::exchange(m_deferredReports, {});
    if (!operation.isValid()) {
        finishRefreshFailure(submitError(result, detail));
        return;
    }
    for (const ParameterService::ExactOperationReport &report : reports) {
        processReport(report);
        if (!guard || m_activity != Activity::Refresh) {
            return;
        }
    }
    if (result != ParameterService::ExactSubmitResult::Started
        && reports.isEmpty()) {
        finishRefreshFailure(submitError(result, detail));
    }
}

void ConfigFFTIntegration::beginWrite(
    quint64 requestId, int componentId,
    const QString &name, const QVariant &value)
{
    const QPointer<ConfigFFTIntegration> guard(this);
    if (componentId != m_target.endpoint.componentId
        || !parameterNames().contains(name)
        || !m_types.contains(name)
        || m_types.value(name) == ParameterType::Unknown) {
        if (m_view) {
            m_view->parameterWriteSubmissionFailed(
                requestId, tr(
                    "The exact parameter type or pinned component is unavailable."));
        }
        return;
    }
    if (!parameterTargetUsable()
        || !m_targets->hasFreshHeartbeat(m_target, 3000)
        || m_targets->heartbeatArmed(m_target)) {
        if (m_view) {
            m_view->parameterWriteSubmissionFailed(
                requestId, tr(
                    "A current fresh and disarmed exact target is required."));
        }
        return;
    }
    QString error;
    if (!beginReservation(Activity::Write, &error)) {
        if (guard && m_view) {
            m_view->parameterWriteSubmissionFailed(
                requestId, error.isEmpty()
                    ? tr("The exact parameter write was rejected.") : error);
        }
        return;
    }
    if (!guard) {
        return;
    }

    m_writeRequestId = requestId;
    m_writeName = name;
    m_writeValue = value;
    ParameterService::ExactWriteRequest request;
    request.name = name;
    request.value = value;
    request.type = m_types.value(name);
    ParameterService::ExactOperationToken operation;
    QString detail;
    m_deferredReports.clear();
    m_submitting = true;
    const ParameterService::ExactSubmitResult result =
        m_parameters->submitExactWrite(
            m_reservation, m_vehicle, request, &operation, &detail);
    if (!guard) {
        return;
    }
    m_submitting = false;
    m_operation = operation;
    m_writeBatchId = operation.operationId;
    if (!operation.isValid()) {
        const QString failure = submitError(result, detail);
        clearActivity();
        if (guard && m_view) {
            m_view->parameterWriteSubmissionFailed(requestId, failure);
        }
        return;
    }
    if (m_view) {
        m_view->parameterWriteSubmitted(requestId, m_writeBatchId);
    }
    if (!guard) {
        return;
    }
    if (m_retired) {
        m_parameters->cancelExactOperation(
            m_reservation, m_operation,
            tr("The selected vehicle changed during parameter write."));
    }
    if (!guard) {
        return;
    }
    const QVector<ParameterService::ExactOperationReport> reports =
        std::exchange(m_deferredReports, {});
    for (const ParameterService::ExactOperationReport &report : reports) {
        processReport(report);
        if (!guard || m_activity != Activity::Write) {
            return;
        }
    }
    if (result != ParameterService::ExactSubmitResult::Started
        && reports.isEmpty()) {
        finishWriteFailure(submitError(result, detail), false);
    }
}

void ConfigFFTIntegration::receiveReport(
    const ParameterService::ExactOperationReport &report)
{
    if (m_destroying || m_activity == Activity::None
        || !m_reservation.isValid()
        || report.token.reservationId != m_reservation.reservationId) {
        return;
    }
    if (m_submitting) {
        m_deferredReports.append(report);
        return;
    }
    if (!m_operation.isValid()
        || report.token.operationId != m_operation.operationId) {
        return;
    }
    processReport(report);
}

void ConfigFFTIntegration::processReport(
    const ParameterService::ExactOperationReport &report)
{
    if (report.token.operationId != m_operation.operationId
        || !report.token.lease.sameInstance(m_vehicle)) {
        return;
    }
    m_operation = {};
    if (m_activity == Activity::Refresh) {
        if (report.terminalResult
            != ParameterService::ExactTerminalResult::ReadSucceeded) {
            finishRefreshFailure(report.description.isEmpty()
                ? tr("An exact FFT parameter could not be read.")
                : report.description);
            return;
        }
        m_refreshValues.append({
            m_target.endpoint.componentId,
            report.token.name, report.value});
        m_types.insert(report.token.name, report.type);
        ++m_refreshIndex;
        if (m_refreshIndex < m_refreshNames.size()) {
            submitNextRead();
            return;
        }
        const QList<ConfigFriendlyParameterValue> values = m_refreshValues;
        const int componentId = m_target.endpoint.componentId;
        const QPointer<ConfigFFTIntegration> guard(this);
        clearActivity();
        if (guard && m_view) {
            m_view->setParameterSnapshot(values, componentId, true);
        }
        return;
    }
    if (m_activity != Activity::Write) {
        return;
    }

    const qulonglong batchId = m_writeBatchId;
    const int componentId = m_target.endpoint.componentId;
    const QString name = m_writeName;
    const QVariant value = report.value.isValid()
        ? report.value : m_writeValue;
    const QString failure = report.description.isEmpty()
        ? tr("The exact FFT parameter write failed.")
        : report.description;
    const bool succeeded = writeSucceeded(report.terminalResult);
    const bool cancelled = report.terminalResult
        == ParameterService::ExactTerminalResult::WriteCancelled;
    const QPointer<ConfigFFTIntegration> guard(this);
    clearActivity();
    if (!guard || !m_view) {
        return;
    }
    if (succeeded) {
        m_view->parameterBatchCompleted(batchId, 1, 0);
        if (guard && m_view) {
            // Batch completion accepts the staged value and clears the view
            // model's pending gate; then publish the exact echoed value.
            m_view->parameterChanged(componentId, name, value);
        }
    } else if (cancelled) {
        m_view->parameterWriteCancelled(batchId, componentId, name);
    } else {
        m_view->parameterWriteFailed(
            batchId, componentId, name, failure);
    }
}

void ConfigFFTIntegration::finishRefreshFailure(const QString &reason)
{
    const QPointer<ConfigFFTView> view = m_view;
    clearActivity();
    if (view) {
        view->refreshFailed(reason);
    }
}

void ConfigFFTIntegration::finishWriteFailure(
    const QString &reason, bool cancelled)
{
    const QPointer<ConfigFFTView> view = m_view;
    const quint64 requestId = m_writeRequestId;
    const qulonglong batchId = m_writeBatchId;
    const int componentId = m_target.endpoint.componentId;
    const QString name = m_writeName;
    clearActivity();
    if (!view) {
        return;
    }
    if (batchId == 0) {
        view->parameterWriteSubmissionFailed(requestId, reason);
    } else if (cancelled) {
        view->parameterWriteCancelled(batchId, componentId, name);
    } else {
        view->parameterWriteFailed(batchId, componentId, name, reason);
    }
}

void ConfigFFTIntegration::clearActivity()
{
    m_operation = {};
    m_activity = Activity::None;
    m_refreshNames.clear();
    m_refreshValues.clear();
    m_refreshIndex = 0;
    m_writeRequestId = 0;
    m_writeBatchId = 0;
    m_writeName.clear();
    m_writeValue.clear();
    m_deferredReports.clear();
    releaseReservation();
}

QString ConfigFFTIntegration::submitError(
    ParameterService::ExactSubmitResult result,
    const QString &detail)
{
    if (!detail.trimmed().isEmpty()) {
        return detail;
    }
    switch (result) {
    case ParameterService::ExactSubmitResult::Started:
        return tr("The exact parameter operation ended unexpectedly.");
    case ParameterService::ExactSubmitResult::InvalidOwner:
    case ParameterService::ExactSubmitResult::InvalidReservation:
        return tr("The exact parameter reservation is invalid.");
    case ParameterService::ExactSubmitResult::InvalidLease:
    case ParameterService::ExactSubmitResult::StaleLease:
        return tr("The pinned vehicle instance is stale.");
    case ParameterService::ExactSubmitResult::InvalidParameter:
        return tr("The FFT parameter or value is invalid.");
    case ParameterService::ExactSubmitResult::RouteUnavailable:
        return tr("The pinned physical route is unavailable.");
    case ParameterService::ExactSubmitResult::Busy:
        return tr("The exact parameter protocol is busy.");
    case ParameterService::ExactSubmitResult::Quarantined:
        return tr("A previous uncertain write is awaiting reconciliation.");
    case ParameterService::ExactSubmitResult::ContextUnavailable:
    case ParameterService::ExactSubmitResult::TransportUnavailable:
        return tr("The exact parameter transport is unavailable.");
    case ParameterService::ExactSubmitResult::TransportOutcomeUncertain:
        return tr("The write transport outcome is uncertain.");
    }
    return tr("The exact parameter operation was rejected.");
}

QString ConfigFFTIntegration::reservationError(
    ParameterService::ExactReservationResult result,
    const QString &detail)
{
    if (!detail.trimmed().isEmpty()) {
        return detail;
    }
    switch (result) {
    case ParameterService::ExactReservationResult::Reserved:
        return QString();
    case ParameterService::ExactReservationResult::InvalidOwner:
        return tr("The FFT parameter owner is invalid.");
    case ParameterService::ExactReservationResult::InvalidLease:
    case ParameterService::ExactReservationResult::StaleLease:
        return tr("The pinned vehicle instance is stale.");
    case ParameterService::ExactReservationResult::RouteUnavailable:
        return tr("The pinned physical route is unavailable.");
    case ParameterService::ExactReservationResult::Busy:
        return tr("The exact parameter protocol is busy.");
    case ParameterService::ExactReservationResult::ContextUnavailable:
        return tr("The exact parameter context is unavailable.");
    }
    return tr("The exact parameter reservation was rejected.");
}

ConfigFFTIntegration *BindConfigFFTViewToExactParameters(
    ConfigFFTView *view,
    ParameterService *parameters,
    VehicleTargetManager *targets,
    ConfigFFTIntegration::ExactLeaseProvider exactLeaseProvider)
{
    if (!view) {
        return nullptr;
    }
    return new ConfigFFTIntegration(
        view, parameters, targets, std::move(exactLeaseProvider), view);
}
