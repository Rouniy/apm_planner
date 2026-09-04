#ifndef CONFIGTRADHELIVIEW_H
#define CONFIGTRADHELIVIEW_H

#include "ConfigTradHeliViewModel.h"

#include <QHash>
#include <QWidget>

class HeliCollectivePlot;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QShowEvent;
class QHideEvent;
class QVBoxLayout;

class ConfigTradHeliView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigTradHeliView(QWidget *parent = nullptr);
    ~ConfigTradHeliView() override;

    QSize sizeHint() const override;
    ConfigTradHeliViewModel *viewModel() const { return m_viewModel; }

    void setCatalog(const ParameterMetaDataCatalog &catalog,
                    bool enforceMetadataRanges = true);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setConnected(bool connected);
    void setArmed(bool armed);
    void setActive(bool active);

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteSubmitted(quint64 requestId, qulonglong batchId);
    void parameterWriteSubmissionFailed(quint64 requestId,
                                        const QString &reason);
    void parameterWriteFailed(qulonglong batchId, int componentId,
                              const QString &name, const QString &reason);
    void parameterBatchCompleted(qulonglong batchId,
                                 int succeeded, int failed);
    void refreshFailed(const QString &reason);
    void refreshCanceled();
    void remoteControlChannelRawChanged(int zeroBasedChannel, float pwm);
    void servoOutputChanged(int oneBasedChannel, int pwm);

signals:
    void writeRequested(quint64 requestId, int componentId,
                        const QString &name, const QVariant &value);
    void refreshRequested(int componentId);
    void safetyWarning(const QString &warning);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct FieldEditors
    {
        QWidget *row = nullptr;
        QLabel *label = nullptr;
        QComboBox *combo = nullptr;
        QDoubleSpinBox *numeric = nullptr;
        QLabel *units = nullptr;
        QLabel *status = nullptr;
    };

    void buildUi();
    void rebuildFields();
    void syncField(const QString &name);
    void syncState();
    void syncVisualization();
    void requestManualMode(int mode);
    const ParamField *findField(const QString &name,
                                const QList<ParamField> &fields) const;
    static QString objectSuffix(const QString &name);

    ConfigTradHeliViewModel *m_viewModel = nullptr;
    QPushButton *m_refresh = nullptr;
    QRadioButton *m_ccpm = nullptr;
    QRadioButton *m_h1 = nullptr;
    QList<QPushButton *> m_manualButtons;
    QLabel *m_status = nullptr;
    QLabel *m_servoStatus = nullptr;
    HeliCollectivePlot *m_plot = nullptr;
    QProgressBar *m_collective = nullptr;
    QLabel *m_collectiveValue = nullptr;
    QProgressBar *m_rudder = nullptr;
    QLabel *m_rudderValue = nullptr;
    QLabel *m_servo1 = nullptr;
    QLabel *m_servo2 = nullptr;
    QLabel *m_servo3 = nullptr;
    QLabel *m_collectiveRange = nullptr;
    QLabel *m_rudderRange = nullptr;
    QLabel *m_safetyWarning = nullptr;
    QWidget *m_fieldsContent = nullptr;
    QVBoxLayout *m_fieldsLayout = nullptr;
    QHash<QString, FieldEditors> m_fieldEditors;
};

#endif // CONFIGTRADHELIVIEW_H
