#ifndef CONFIGFFTVIEW_H
#define CONFIGFFTVIEW_H

#include "ConfigFFTViewModel.h"

#include <QPointer>
#include <QVector>
#include <QWidget>

class QAction;
class QCheckBox;
class QCustomPlot;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QToolButton;

class ConfigFFTView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigFFTView(ConfigFFTViewModel *viewModel = nullptr,
                           QWidget *parent = nullptr);
    ~ConfigFFTView() override;

    ConfigFFTViewModel *viewModel() const { return m_viewModel; }
    QCustomPlot *plot() const { return m_plot; }

    void setCatalog(const ParameterMetaDataCatalog &catalog,
                    bool enforceMetadataRanges = true);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1, bool completeSnapshot = true);
    void setParameterContext(bool connected, bool heartbeatFresh,
                             bool armed);
    void setConnected(bool connected);
    void setHeartbeatFresh(bool fresh);
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
                        QString name, QVariant value);
    void refreshRequested(int componentId);

private:
    struct FieldEditors
    {
        QLabel *label = nullptr;
        QDoubleSpinBox *numeric = nullptr;
        QToolButton *bitmask = nullptr;
        QVector<QAction *> bitActions;
        QLabel *units = nullptr;
        QLabel *status = nullptr;
    };

    void buildUi();
    void rebuildFields();
    void syncState();
    void syncPlot();
    void chooseLogFile();
    void stageBitmask(int fieldIndex);
    static QString bitmaskSummary(const ParamField &field);

    QPointer<ConfigFFTViewModel> m_viewModel;
    QLabel *m_title = nullptr;
    QLabel *m_info = nullptr;
    QLabel *m_parameterStatus = nullptr;
    QPushButton *m_refresh = nullptr;
    QPushButton *m_fft = nullptr;
    QPushButton *m_cancel = nullptr;
    QSpinBox *m_bins = nullptr;
    QDoubleSpinBox *m_startFrequency = nullptr;
    QCheckBox *m_magnitude = nullptr;
    QLabel *m_fftStatus = nullptr;
    QLabel *m_plotTitle = nullptr;
    QCustomPlot *m_plot = nullptr;
    QWidget *m_fieldsHost = nullptr;
    QVector<FieldEditors> m_fieldEditors;
};

#endif // CONFIGFFTVIEW_H
