#ifndef CONFIGFRAMECLASSTYPEVIEW_H
#define CONFIGFRAMECLASSTYPEVIEW_H

#include "ConfigFrameClassTypeViewModel.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;

class ConfigFrameClassTypeView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigFrameClassTypeView(QWidget *parent = nullptr);
    ~ConfigFrameClassTypeView() override;

    QSize sizeHint() const override;
    ConfigFrameClassTypeViewModel *viewModel() const { return m_viewModel; }

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
    void syncOptions();
    void syncState();
    static int optionIndex(const QComboBox *combo,
                           const QVariant &value);

    ConfigFrameClassTypeViewModel *m_viewModel = nullptr;
    QPushButton *m_refresh = nullptr;
    QComboBox *m_frameClass = nullptr;
    QComboBox *m_frameType = nullptr;
    QLabel *m_preview = nullptr;
    QLabel *m_previewCaption = nullptr;
    QLabel *m_status = nullptr;
};

#endif // CONFIGFRAMECLASSTYPEVIEW_H
