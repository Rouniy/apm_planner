#ifndef CONFIGFRAMETYPEVIEW_H
#define CONFIGFRAMETYPEVIEW_H

#include "ConfigFrameTypeViewModel.h"

#include <QHash>
#include <QWidget>

class QLabel;
class QRadioButton;

class ConfigFrameTypeView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigFrameTypeView(QWidget *parent = nullptr);
    ~ConfigFrameTypeView() override;

    QSize sizeHint() const override;
    ConfigFrameTypeViewModel *viewModel() const { return m_viewModel; }

    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setConnected(bool connected);
    void setArmed(bool armed);

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteSubmitted(quint64 requestId, qulonglong batchId);
    void parameterWriteSubmissionFailed(quint64 requestId,
                                        const QString &reason);
    void parameterWriteFailed(qulonglong batchId, int componentId,
                              const QString &name, const QString &reason);
    void parameterWriteCancelled(qulonglong batchId, int componentId,
                                 const QString &name);
    void parameterBatchCompleted(qulonglong batchId,
                                 int succeeded, int failed);
    void refreshFailed(const QString &reason);
    void refreshCanceled();

signals:
    void writeRequested(quint64 requestId, int componentId,
                        QVariantList changes);
    void refreshRequested(int componentId);

private:
    void buildUi();
    void syncState();

    ConfigFrameTypeViewModel *m_viewModel = nullptr;
    QHash<int, QRadioButton *> m_radios;
    QLabel *m_status = nullptr;
};

#endif // CONFIGFRAMETYPEVIEW_H
