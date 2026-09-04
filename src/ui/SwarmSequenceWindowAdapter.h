#ifndef SWARMSEQUENCEWINDOWADAPTER_H
#define SWARMSEQUENCEWINDOWADAPTER_H

#include "SwarmSequenceWindow.h"

#include <QPointer>

class SwarmSequenceExecutor;

/** Production window adapter for the application-owned Sequence executor. */
class SwarmSequenceWindowAdapter final
    : public SwarmSequenceWindowInterface
{
public:
    explicit SwarmSequenceWindowAdapter(
        SwarmSequenceExecutor *executor, QObject *parent = nullptr);
    ~SwarmSequenceWindowAdapter() override;

    bool executorReady(QString *error) const override;
    bool prepareRunStep(
        const SwarmSequenceRunStepRequest &request,
        SwarmSequencePreparedRunStep *prepared, QString *error) const override;
    bool runStep(const SwarmSequencePreparedRunStep &prepared,
                 QString *error) override;
    bool prepareTakeoff(
        const QVector<SwarmSequenceTakeoffAssignment> &assignments,
        SwarmSequencePreparedTakeoff *prepared, QString *error) const override;
    bool startTakeoff(const SwarmSequencePreparedTakeoff &prepared,
                      QString *error) override;
    void cancelActiveOperation(const QString &reason) override;
    bool isActive() const noexcept override;
    SwarmSequenceExecutor::State state() const noexcept override;
    QString statusText() const override;
    quint64 operationGeneration() const noexcept override;
    SwarmSequenceOperationReport lastReport() const override;
    void setChangedHandler(ChangedHandler handler) override;

private:
    void scheduleChanged();

    QPointer<SwarmSequenceExecutor> m_executor;
    ChangedHandler m_changedHandler;
    bool m_changeQueued = false;
};

#endif // SWARMSEQUENCEWINDOWADAPTER_H
