#include "QGCUASParamManager.h"

#include "comm/ParameterService.h"
#include "comm/VehicleTargetManager.h"
#include "core/parameters/ParameterStore.h"

#include <algorithm>
#include <QPointer>
#include <QTimer>
#include <utility>

QGCUASParamManager::QGCUASParamManager(
    ParameterService *service,
    VehicleTargetManager *targetManager,
    QObject *parent)
    : QObject(parent)
    , m_service(service)
    , m_targetManager(targetManager)
    , m_store(service ? service->store() : nullptr)
{
    Q_ASSERT(m_service);
    Q_ASSERT(m_targetManager);
    Q_ASSERT(m_store);

    // currentTargetChanged also reports display-name metadata updates. Only
    // a generation change invalidates the exact endpoint and its parameter
    // transaction/snapshot state.
    connect(m_targetManager, &VehicleTargetManager::targetGenerationChanged,
            this, &QGCUASParamManager::handleTargetChanged);
    connect(m_service, &ParameterService::listStarted,
            this, &QGCUASParamManager::handleListStarted);
    connect(m_service, &ParameterService::listCompleted,
            this, &QGCUASParamManager::handleListCompleted);
    connect(m_service, &ParameterService::listFailed,
            this, &QGCUASParamManager::handleListFailed);
    connect(m_service, &ParameterService::listCancelled,
            this, &QGCUASParamManager::handleListCancelled);
    connect(m_service, &ParameterService::parameterValueReceived,
            this, &QGCUASParamManager::handleParameterValue);
    connect(m_service, &ParameterService::parameterWriteAcknowledged,
            this, &QGCUASParamManager::handleParameterWriteAcknowledged);
    connect(m_service, &ParameterService::parameterWriteStarted,
            this,
            [this](qulonglong transactionId, qulonglong batchId,
                   qulonglong generation,
                   int linkId, int systemId, int componentId,
                   const QString &name, const QVariant &value, int type) {
        if (matchesCurrentTarget(
                generation, linkId, systemId, componentId)) {
            emit parameterWriteStarted(
                transactionId, batchId, componentId, name, value, type);
        }
    });
    connect(m_service, &ParameterService::parameterWriteRetried,
            this, &QGCUASParamManager::parameterWriteRetried);
    connect(m_service, &ParameterService::parameterWriteFailed,
            this,
            [this](qulonglong transactionId, qulonglong batchId,
                   qulonglong generation,
                   int linkId, int systemId, int componentId,
                   const QString &name, int reason,
                   const QString &message) {
        if (matchesCurrentTarget(
                generation, linkId, systemId, componentId)) {
            emit parameterWriteFailed(
                transactionId, batchId, componentId,
                name, reason, message);
        }
    });
    connect(m_service, &ParameterService::parameterWriteCancelled,
            this,
            [this](qulonglong transactionId, qulonglong batchId,
                   qulonglong generation,
                   int linkId, int systemId, int componentId,
                   const QString &name) {
        if (matchesCurrentTarget(
                generation, linkId, systemId, componentId)) {
            emit parameterWriteCancelled(
                transactionId, batchId, componentId, name);
        }
    });
    connect(m_service, &ParameterService::parameterBatchStarted,
            this,
            [this](qulonglong batchId, qulonglong generation,
                   int linkId, int systemId, int componentId, int total) {
        VehicleTargetLease batchTarget;
        batchTarget.generation = generation;
        batchTarget.endpoint.linkId = linkId;
        batchTarget.endpoint.systemId = systemId;
        batchTarget.endpoint.componentId = componentId;
        m_parameterBatchTargets.insert(batchId, batchTarget);
        if (matchesCurrentTarget(
                generation, linkId, systemId, componentId)) {
            emit parameterBatchStarted(batchId, total);
        }
    });
    connect(m_service, &ParameterService::parameterBatchProgress,
            this,
            [this](qulonglong batchId, int completed, int total,
                   int succeeded, int failed) {
        const VehicleTargetLease target =
            m_parameterBatchTargets.value(batchId);
        if (target.isValid()
            && matchesCurrentTarget(
                target.generation, target.endpoint.linkId,
                target.endpoint.systemId, target.endpoint.componentId)) {
            emit parameterBatchProgress(
                batchId, completed, total, succeeded, failed);
        }
    });
    connect(m_service, &ParameterService::parameterBatchCompleted,
            this,
            [this](qulonglong batchId, int succeeded, int failed) {
        const VehicleTargetLease target =
            m_parameterBatchTargets.take(batchId);
        if (target.isValid()
            && matchesCurrentTarget(
                target.generation, target.endpoint.linkId,
                target.endpoint.systemId, target.endpoint.componentId)) {
            emit parameterBatchCompleted(batchId, succeeded, failed);
        }
    });
    connect(m_store, &ParameterStore::progressChanged,
            this, &QGCUASParamManager::handleProgress);
    handleTargetChanged(m_targetManager->targetGeneration());
}

QList<int> QGCUASParamManager::getComponentIds() const
{
    QList<int> result;
    if (!m_store) {
        return result;
    }
    for (const ParameterRecord &record : m_store->snapshot().records()) {
        if (!result.contains(record.key.componentId)) {
            result.append(record.key.componentId);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

QList<QString> QGCUASParamManager::getParameterNames(int component) const
{
    QList<QString> result;
    if (!m_store || component < 0 || component > 255) {
        return result;
    }
    for (const ParameterRecord &record : m_store->snapshot().records()) {
        if (record.key.componentId == component) {
            result.append(record.key.name);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

QList<QVariant> QGCUASParamManager::getParameterValues(int component) const
{
    QList<QVariant> result;
    if (!m_store || component < 0 || component > 255) {
        return result;
    }
    QList<ParameterRecord> records;
    for (const ParameterRecord &record : m_store->snapshot().records()) {
        if (record.key.componentId == component) {
            records.append(record);
        }
    }
    std::sort(records.begin(), records.end(),
              [](const ParameterRecord &left,
                 const ParameterRecord &right) {
        return left.key.name < right.key.name;
    });
    for (const ParameterRecord &record : records) {
        result.append(record.value);
    }
    return result;
}

bool QGCUASParamManager::getParameterValue(
    int component, const QString &parameter, QVariant &value) const
{
    if (!m_store || component < 0 || component > 255) {
        return false;
    }
    const ParameterSnapshot snapshot = m_store->snapshot();
    if (!snapshot.contains(static_cast<quint8>(component), parameter)) {
        return false;
    }
    value = snapshot.value(
        static_cast<quint8>(component), parameter).value;
    return true;
}

QVariant QGCUASParamManager::getParameterValue(
    int component, const QString &parameter) const
{
    QVariant value;
    return getParameterValue(component, parameter, value) ? value : QVariant{};
}

bool QGCUASParamManager::isParamMinKnown(const QString &param) const
{
    return m_paramMin.contains(param);
}

bool QGCUASParamManager::isParamMaxKnown(const QString &param) const
{
    return m_paramMax.contains(param);
}

bool QGCUASParamManager::isParamDefaultKnown(const QString &param) const
{
    return m_paramDefault.contains(param);
}

double QGCUASParamManager::getParamMin(const QString &param) const
{
    return m_paramMin.value(param, 0.0);
}

double QGCUASParamManager::getParamMax(const QString &param) const
{
    return m_paramMax.value(param, 0.0);
}

double QGCUASParamManager::getParamDefault(const QString &param) const
{
    return m_paramDefault.value(param, 0.0);
}

QString QGCUASParamManager::getParamInfo(const QString &param) const
{
    return m_paramToolTips.value(param);
}

void QGCUASParamManager::setParamInfo(
    const QMap<QString, QString> &param)
{
    m_paramToolTips = param;
}

void QGCUASParamManager::setParamMetadata(
    const QMap<QString, double> &minimum,
    const QMap<QString, double> &maximum,
    const QMap<QString, double> &defaults,
    const QMap<QString, QString> &tooltips)
{
    m_paramMin = minimum;
    m_paramMax = maximum;
    m_paramDefault = defaults;
    m_paramToolTips = tooltips;
}

qulonglong QGCUASParamManager::writeParameters(
    int component, const QVariantList &changes, bool force)
{
    if (!m_service || !m_targetManager || component < 0 || component > 255) {
        return 0;
    }
    const VehicleTargetLease target = m_targetManager->acquireTarget();
    if (!target.isValid() || target.endpoint.componentId != component) {
        return 0;
    }
    return m_service->writeCurrentParameters(changes, force);
}

void QGCUASParamManager::setParameter(
    int component, QString parameterName, QVariant value)
{
    if (!m_service || !m_targetManager || !m_store
        || component < 0 || component > 255) {
        return;
    }
    const VehicleTargetLease target = m_targetManager->acquireTarget();
    if (!target.isValid() || target.endpoint.componentId != component) {
        return;
    }
    const ParameterSnapshot snapshot = m_store->snapshot(target.endpoint);
    if (!snapshot.contains(static_cast<quint8>(component), parameterName)) {
        // Mission Planner rejects writes for parameters absent from the
        // committed target cache; guessing a wire type is unsafe.
        return;
    }
    m_service->writeCurrentParameter(parameterName, value);
}

void QGCUASParamManager::requestParameterList()
{
    // Setup, Config and trusted plugins share this facade. Suppress recursive
    // requests and coalesce retries for the remainder of an event-loop turn
    // only after a synchronous failure. Successful explicit refreshes remain
    // available immediately.
    const VehicleTargetLease requestedTarget = m_targetManager
        ? m_targetManager->acquireTarget() : VehicleTargetLease{};
    const bool repeatedFailedTarget = m_parameterListRequestFailedThisTurn
        && m_failedParameterListTarget.generation
            == requestedTarget.generation
        && m_failedParameterListTarget.endpoint.sameIdentity(
            requestedTarget.endpoint);
    if (m_parameterListRequestDispatching) {
        if (requestedTarget.isValid()
            && (requestedTarget.generation
                    != m_dispatchedParameterListTarget.generation
                || !requestedTarget.endpoint.sameIdentity(
                    m_dispatchedParameterListTarget.endpoint))) {
            deferParameterListRequest(requestedTarget);
        }
        return;
    }
    if (repeatedFailedTarget) {
        return;
    }
    if (m_parameterListRequestFailedThisTurn && !repeatedFailedTarget) {
        m_parameterListRequestFailedThisTurn = false;
        m_failedParameterListTarget = {};
    }
    const auto rememberFailure = [this, requestedTarget]() {
        m_parameterListRequestFailedThisTurn = true;
        m_failedParameterListTarget = requestedTarget;
        QTimer::singleShot(0, this, [this, requestedTarget]() {
            if (m_failedParameterListTarget.generation
                    != requestedTarget.generation
                || !m_failedParameterListTarget.endpoint.sameIdentity(
                    requestedTarget.endpoint)) {
                return;
            }
            m_parameterListRequestFailedThisTurn = false;
            m_failedParameterListTarget = {};
        });
    };
    if (!m_service) {
        rememberFailure();
        emit parameterListLoadFailed(
            QStringLiteral("Parameter service is unavailable."));
        return;
    }
    const QPointer<QGCUASParamManager> guard(this);
    m_parameterListRequestDispatching = true;
    m_dispatchedParameterListTarget = requestedTarget;
    const auto result = static_cast<ParameterService::SendResult>(
        m_service->requestCurrentParameterList());
    if (!guard) {
        return;
    }
    m_parameterListRequestDispatching = false;
    m_dispatchedParameterListTarget = {};
    if (result != ParameterService::SendResult::Sent) {
        rememberFailure();
    }
    if (result == ParameterService::SendResult::InvalidTarget) {
        emit parameterListLoadFailed(
            QStringLiteral("No exact vehicle target is selected."));
    } else if (result == ParameterService::SendResult::StaleTarget) {
        emit parameterListLoadFailed(
            QStringLiteral("The selected vehicle target changed."));
    }
}

void QGCUASParamManager::deferParameterListRequest(
    const VehicleTargetLease &target)
{
    m_deferredParameterListTarget = target;
    if (m_deferredParameterListScheduled) {
        return;
    }
    m_deferredParameterListScheduled = true;
    QTimer::singleShot(
        0, this, &QGCUASParamManager::dispatchDeferredParameterListRequest);
}

void QGCUASParamManager::dispatchDeferredParameterListRequest()
{
    m_deferredParameterListScheduled = false;
    const VehicleTargetLease requestedTarget = m_deferredParameterListTarget;
    m_deferredParameterListTarget = {};
    if (!requestedTarget.isValid() || !m_targetManager
        || !m_targetManager->isCurrentTarget(
            requestedTarget.endpoint.linkId,
            requestedTarget.endpoint.systemId,
            requestedTarget.endpoint.componentId,
            requestedTarget.generation)) {
        return;
    }
    requestParameterList();
}

void QGCUASParamManager::cancelParameterList()
{
    if (m_service) {
        m_service->cancelCurrentParameterList();
    }
}

void QGCUASParamManager::requestParameterListUpdate(int component)
{
    Q_UNUSED(component)
    requestParameterList();
}

void QGCUASParamManager::requestParameterUpdate(
    int component, const QString &parameter)
{
    if (!m_service || !m_targetManager || component < 0 || component > 255) {
        return;
    }
    const VehicleTargetLease target = m_targetManager->acquireTarget();
    if (!target.isValid() || target.endpoint.componentId != component) {
        return;
    }
    const auto result = static_cast<ParameterService::SendResult>(
        m_service->requestCurrentParameterRead(parameter));
    if (result == ParameterService::SendResult::StaleTarget
        && !m_targetManager->isTargetGenerationSettled()) {
        deferParameterReadRequest(target, parameter);
    }
}

void QGCUASParamManager::deferParameterReadRequest(
    const VehicleTargetLease &target, const QString &name)
{
    for (const DeferredParameterRead &request : m_deferredParameterReads) {
        if (request.target.generation == target.generation
            && request.target.endpoint.sameIdentity(target.endpoint)
            && request.name == name) {
            return;
        }
    }
    m_deferredParameterReads.append({target, name});
    if (m_deferredParameterReadsScheduled) {
        return;
    }
    m_deferredParameterReadsScheduled = true;
    QTimer::singleShot(
        0, this,
        &QGCUASParamManager::dispatchDeferredParameterReadRequests);
}

void QGCUASParamManager::dispatchDeferredParameterReadRequests()
{
    m_deferredParameterReadsScheduled = false;
    const QList<DeferredParameterRead> requests =
        std::exchange(m_deferredParameterReads, {});
    for (const DeferredParameterRead &request : requests) {
        if (!m_targetManager->isCurrentTarget(
                request.target.endpoint.linkId,
                request.target.endpoint.systemId,
                request.target.endpoint.componentId,
                request.target.generation)) {
            continue;
        }
        requestParameterUpdate(
            request.target.endpoint.componentId, request.name);
    }
}

bool QGCUASParamManager::matchesCurrentTransaction(
    qulonglong generation, int linkId, int systemId, int componentId) const
{
    return m_listGeneration == generation
        && m_listEndpoint.linkId == linkId
        && m_listEndpoint.systemId == systemId
        && m_listEndpoint.componentId == componentId;
}

bool QGCUASParamManager::matchesCurrentTarget(
    qulonglong generation, int linkId, int systemId, int componentId) const
{
    const VehicleTargetLease current = m_targetManager->acquireTarget();
    return current.isValid()
        && current.generation == generation
        && current.endpoint.linkId == linkId
        && current.endpoint.systemId == systemId
        && current.endpoint.componentId == componentId;
}

void QGCUASParamManager::setParameterListReady(bool ready)
{
    if (m_parameterListReady == ready) {
        return;
    }
    m_parameterListReady = ready;
    emit parameterListReadyChanged(ready);
}

bool QGCUASParamManager::replayCommittedSnapshot(
    const VehicleTargetLease &target)
{
    if (!m_store || !target.isValid()
        || !matchesCurrentTarget(
            target.generation, target.endpoint.linkId,
            target.endpoint.systemId, target.endpoint.componentId)) {
        return false;
    }
    QList<ParameterRecord> records =
        m_store->snapshot(target.endpoint).records();
    std::sort(records.begin(), records.end(),
              [](const ParameterRecord &left,
                 const ParameterRecord &right) {
        if (left.key.componentId != right.key.componentId) {
            return left.key.componentId < right.key.componentId;
        }
        return left.key.name < right.key.name;
    });
    for (const ParameterRecord &record : records) {
        if (!matchesCurrentTarget(
                target.generation, target.endpoint.linkId,
                target.endpoint.systemId, target.endpoint.componentId)) {
            return false;
        }
        emit parameterChanged(record.key.componentId,
                              record.key.name, record.value);
        if (!matchesCurrentTarget(
                target.generation, target.endpoint.linkId,
                target.endpoint.systemId, target.endpoint.componentId)) {
            return false;
        }
        emit parameterChanged(record.key.componentId,
                              record.index, record.value);
        if (!matchesCurrentTarget(
                target.generation, target.endpoint.linkId,
                target.endpoint.systemId, target.endpoint.componentId)) {
            return false;
        }
        emit parameterValueReceived(
            record.key.componentId, record.reportedCount, record.index,
            record.key.name, record.value,
            static_cast<int>(record.type));
    }
    return matchesCurrentTarget(
        target.generation, target.endpoint.linkId,
        target.endpoint.systemId, target.endpoint.componentId);
}

void QGCUASParamManager::handleTargetChanged(qulonglong generation)
{
    if (generation != m_targetManager->targetGeneration()
        || generation == m_lastHandledTargetGeneration) {
        return;
    }
    m_lastHandledTargetGeneration = generation;
    const VehicleTargetLease expectedTarget =
        m_targetManager->acquireTarget();
    m_parameterListRequestFailedThisTurn = false;
    m_failedParameterListTarget = {};
    // ParameterService receives targetGenerationChanged before this facade.
    // A trusted service consumer may synchronously start the new target list
    // from the old list's cancellation callback; retain that transaction.
    const bool newTargetListAlreadyStarted = m_parameterListInProgress
        && matchesCurrentTarget(
            m_listGeneration, m_listEndpoint.linkId,
            m_listEndpoint.systemId, m_listEndpoint.componentId);
    if (!newTargetListAlreadyStarted) {
        m_parameterListInProgress = false;
        m_listGeneration = 0;
        m_listEndpoint = {};
    }
    const ParameterSnapshot snapshot =
        m_store && expectedTarget.isValid()
        ? m_store->snapshot(expectedTarget.endpoint) : ParameterSnapshot{};
    const ParameterProgress progress = snapshot.progress();
    m_parameterListReceivedCount = progress.received;
    m_parameterListReportedCount = progress.reported;
    const bool ready = !m_parameterListInProgress
        && expectedTarget.isValid() && snapshot.isComplete();
    const bool readinessChanged = m_parameterListReady != ready;
    m_parameterListReady = ready;
    // Invalidate consumers before any new-target readiness or replay signal
    // can be observed. This also prevents old staged edits crossing targets.
    emit parameterTargetChanged();
    if (generation != m_targetManager->targetGeneration()) {
        return;
    }
    emit parameterSnapshotAboutToChange();
    if (generation != m_targetManager->targetGeneration()) {
        return;
    }
    if (readinessChanged) {
        emit parameterListReadyChanged(m_parameterListReady);
        if (generation != m_targetManager->targetGeneration()) {
            return;
        }
    }
    if (m_parameterListReady) {
        replayCommittedSnapshot(expectedTarget);
    }
}

void QGCUASParamManager::handleListStarted(
    qulonglong generation, int linkId, int systemId, int componentId)
{
    if (!matchesCurrentTarget(
            generation, linkId, systemId, componentId)) {
        return;
    }
    m_parameterListInProgress = true;
    m_parameterListReceivedCount = 0;
    m_parameterListReportedCount = 0;
    m_listGeneration = generation;
    m_listEndpoint.linkId = linkId;
    m_listEndpoint.systemId = systemId;
    m_listEndpoint.componentId = componentId;
    setParameterListReady(false);
    if (!matchesCurrentTarget(
            generation, linkId, systemId, componentId)) {
        return;
    }
    emit parameterListLoadStarted();
}

void QGCUASParamManager::handleListCompleted(
    qulonglong generation, int linkId, int systemId, int componentId)
{
    if (!matchesCurrentTransaction(
            generation, linkId, systemId, componentId)
        || !matchesCurrentTarget(
            generation, linkId, systemId, componentId)) {
        return;
    }
    m_parameterListInProgress = false;
    m_listGeneration = 0;
    m_listEndpoint = {};
    const ParameterProgress progress = m_store->progress();
    m_parameterListReceivedCount = progress.received;
    m_parameterListReportedCount = progress.reported;
    emit parameterSnapshotAboutToChange();
    if (!matchesCurrentTarget(
            generation, linkId, systemId, componentId)) {
        return;
    }
    setParameterListReady(m_store->specializedPagesReady());
    if (!matchesCurrentTarget(
            generation, linkId, systemId, componentId)) {
        return;
    }
    VehicleTargetLease completedTarget;
    completedTarget.generation = generation;
    completedTarget.endpoint.linkId = linkId;
    completedTarget.endpoint.systemId = systemId;
    completedTarget.endpoint.componentId = componentId;
    if (!replayCommittedSnapshot(completedTarget)) {
        return;
    }
    emit parameterListUpToDate(componentId);
}

void QGCUASParamManager::handleListFailed(
    qulonglong generation, int linkId, int systemId, int componentId,
    const QString &reason)
{
    if (!matchesCurrentTransaction(
            generation, linkId, systemId, componentId)
        || !matchesCurrentTarget(
            generation, linkId, systemId, componentId)) {
        return;
    }
    m_parameterListInProgress = false;
    m_listGeneration = 0;
    m_listEndpoint = {};
    setParameterListReady(m_store->specializedPagesReady());
    if (!matchesCurrentTarget(
            generation, linkId, systemId, componentId)) {
        return;
    }
    emit parameterListLoadFailed(reason);
}

void QGCUASParamManager::handleListCancelled(
    qulonglong generation, int linkId, int systemId, int componentId)
{
    if (!matchesCurrentTransaction(
            generation, linkId, systemId, componentId)
        || !matchesCurrentTarget(
            generation, linkId, systemId, componentId)) {
        return;
    }
    m_parameterListInProgress = false;
    m_listGeneration = 0;
    m_listEndpoint = {};
    setParameterListReady(m_store->specializedPagesReady());
    if (!matchesCurrentTarget(
            generation, linkId, systemId, componentId)) {
        return;
    }
    emit parameterListLoadCanceled();
}

void QGCUASParamManager::handleParameterValue(
    qulonglong generation,
    int linkId, int systemId, int componentId,
    int parameterCount, int parameterIndex,
    const QString &name, const QVariant &value, int type)
{
    Q_UNUSED(parameterCount)
    Q_UNUSED(type)
    if (m_parameterListInProgress
        && matchesCurrentTransaction(
            generation, linkId, systemId, componentId)) {
        // Staged values are deliberately hidden. The old committed snapshot
        // stays visible until listCompleted atomically replaces it.
        return;
    }
    const VehicleTargetLease current = m_targetManager->acquireTarget();
    if (!current.isValid() || current.generation != generation
        || current.endpoint.linkId != linkId
        || current.endpoint.systemId != systemId
        || current.endpoint.componentId != componentId) {
        return;
    }
    emit parameterChanged(componentId, name, value);
    if (!matchesCurrentTarget(
            generation, linkId, systemId, componentId)) {
        return;
    }
    emit parameterChanged(componentId, parameterIndex, value);
    if (!matchesCurrentTarget(
            generation, linkId, systemId, componentId)) {
        return;
    }
    emit parameterValueReceived(componentId, parameterCount, parameterIndex,
                                name, value, type);
}

void QGCUASParamManager::handleParameterWriteAcknowledged(
    qulonglong generation,
    int linkId, int systemId, int componentId,
    const QString &name, const QVariant &value, int type)
{
    const VehicleTargetLease current = m_targetManager->acquireTarget();
    if (!current.isValid() || current.generation != generation
        || current.endpoint.linkId != linkId
        || current.endpoint.systemId != systemId
        || current.endpoint.componentId != componentId) {
        return;
    }
    emit parameterWriteAcknowledged(componentId, name, value, type);
}

void QGCUASParamManager::handleProgress(
    int received, int reported, int percent)
{
    if (!m_targetManager->isTargetGenerationSettled()) {
        return;
    }
    Q_UNUSED(percent)
    m_parameterListReceivedCount = received;
    m_parameterListReportedCount = reported;
    emit parameterListProgressChanged(received, reported, percent);
}
