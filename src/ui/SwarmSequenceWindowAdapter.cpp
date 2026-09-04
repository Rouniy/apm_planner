#include "SwarmSequenceWindowAdapter.h"

#include "services/SwarmSequenceExecutor.h"

#include <QTimer>

#include <utility>

namespace
{
bool unavailable(QString *error)
{
    if (error) {
        *error = QStringLiteral("The exact Sequence executor is unavailable.");
    }
    return false;
}
}

SwarmSequenceWindowAdapter::SwarmSequenceWindowAdapter(
    SwarmSequenceExecutor *executor, QObject *parent)
    : SwarmSequenceWindowInterface(parent)
    , m_executor(executor)
{
    if (m_executor) {
        connect(m_executor, &SwarmSequenceExecutor::changed,
                this, &SwarmSequenceWindowAdapter::scheduleChanged);
        connect(m_executor, &QObject::destroyed,
                this, &SwarmSequenceWindowAdapter::scheduleChanged);
    }
}

SwarmSequenceWindowAdapter::~SwarmSequenceWindowAdapter()
{
    m_changedHandler = ChangedHandler();
}

bool SwarmSequenceWindowAdapter::executorReady(QString *error) const
{
    return m_executor ? m_executor->executorReady(error) : unavailable(error);
}

bool SwarmSequenceWindowAdapter::prepareRunStep(
    const SwarmSequenceRunStepRequest &request,
    SwarmSequencePreparedRunStep *prepared, QString *error) const
{
    return m_executor
        ? m_executor->prepareRunStep(request, prepared, error)
        : unavailable(error);
}

bool SwarmSequenceWindowAdapter::runStep(
    const SwarmSequencePreparedRunStep &prepared, QString *error)
{
    return m_executor ? m_executor->runStep(prepared, error)
                      : unavailable(error);
}

bool SwarmSequenceWindowAdapter::prepareTakeoff(
    const QVector<SwarmSequenceTakeoffAssignment> &assignments,
    SwarmSequencePreparedTakeoff *prepared, QString *error) const
{
    return m_executor
        ? m_executor->prepareTakeoff(assignments, prepared, error)
        : unavailable(error);
}

bool SwarmSequenceWindowAdapter::startTakeoff(
    const SwarmSequencePreparedTakeoff &prepared, QString *error)
{
    return m_executor ? m_executor->startTakeoff(prepared, error)
                      : unavailable(error);
}

void SwarmSequenceWindowAdapter::cancelActiveOperation(
    const QString &reason)
{
    if (m_executor) {
        m_executor->cancelActiveOperation(reason);
    }
}

bool SwarmSequenceWindowAdapter::isActive() const noexcept
{
    return m_executor && m_executor->isActive();
}

SwarmSequenceExecutor::State
SwarmSequenceWindowAdapter::state() const noexcept
{
    return m_executor ? m_executor->state()
                      : SwarmSequenceExecutor::State::OutcomeUncertain;
}

QString SwarmSequenceWindowAdapter::statusText() const
{
    return m_executor ? m_executor->statusText()
                      : QStringLiteral("The exact Sequence executor is unavailable.");
}

quint64 SwarmSequenceWindowAdapter::operationGeneration() const noexcept
{
    return m_executor ? m_executor->operationGeneration() : 0;
}

SwarmSequenceOperationReport
SwarmSequenceWindowAdapter::lastReport() const
{
    return m_executor ? m_executor->lastReport()
                      : SwarmSequenceOperationReport();
}

void SwarmSequenceWindowAdapter::setChangedHandler(ChangedHandler handler)
{
    m_changedHandler = std::move(handler);
}

void SwarmSequenceWindowAdapter::scheduleChanged()
{
    if (m_changeQueued) {
        return;
    }
    m_changeQueued = true;
    QTimer::singleShot(0, this, [this]() {
        m_changeQueued = false;
        if (m_changedHandler) {
            m_changedHandler();
        }
    });
}
