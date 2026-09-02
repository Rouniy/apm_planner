#ifndef CONFIGHWOSDVIEW_H
#define CONFIGHWOSDVIEW_H

#include "ConfigHWOSDViewModel.h"

#include <QWidget>

class QLabel;
class QPushButton;

/*
 * Mission Planner 10 SETUP > OSD page (ConfigHWOSDView.axaml): title "OSD",
 * intro, the "Enable Telemetry" button with its status line, the note, and
 * the MinimOSD picture of the original page. Everything else lives in
 * ConfigHWOSDViewModel; transport wiring belongs to the owner.
 */
class ConfigHWOSDView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigHWOSDView(QWidget *parent = nullptr);
    ~ConfigHWOSDView() override;

    QSize sizeHint() const override;
    ConfigHWOSDViewModel *viewModel() const { return m_viewModel; }

    void setParameterSnapshot(const QList<ConfigFriendlyParameterValue> &parameters,
                              int preferredComponent = 1);
    void setConnected(bool connected);

public slots:
    void parameterTargetChanged();
    void enableTelemetry();
    // Batch lifecycle from the owner of the parameter transaction.
    void parameterBatchSubmitted(int componentId, qulonglong batchId);
    void parameterBatchProgress(qulonglong batchId, int completed, int total, int succeeded,
                                int failed);
    void parameterWriteFailed(qulonglong batchId, int componentId, const QString &name,
                              const QString &reason);
    void parameterBatchCompleted(qulonglong batchId, int succeeded, int failed);
    void parameterBatchCancelled(qulonglong batchId);
    void parameterWriteSubmissionFailed(const QString &reason);

signals:
    // "Enable Telemetry": one batch of {name, value = 2} for the present parameters.
    void writeParamsRequested(int componentId, QVariantList changes);

private:
    void buildUi();
    void syncState();

    ConfigHWOSDViewModel *m_viewModel = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_intro = nullptr;
    QPushButton *m_enableButton = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_note = nullptr;
};

#endif // CONFIGHWOSDVIEW_H
