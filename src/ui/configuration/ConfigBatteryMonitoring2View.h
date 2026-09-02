#ifndef CONFIGBATTERYMONITORING2VIEW_H
#define CONFIGBATTERYMONITORING2VIEW_H

#include "ConfigBatteryMonitoring2ViewModel.h"
#include "ParamField.h"

#include <QWidget>

class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

class ConfigBatteryMonitoring2View final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigBatteryMonitoring2View(
        const ParameterMetaDataCatalog &catalog = ParameterMetaDataCatalog(),
        QWidget *parent = nullptr);
    ~ConfigBatteryMonitoring2View() override = default;

    ConfigBatteryMonitoring2ViewModel *viewModel() const
    {
        return m_viewModel;
    }

    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = MAV_COMP_ID_PRIMARY);
    void setCatalog(const ParameterMetaDataCatalog &catalog);
    void setConnected(bool connected);
    void observeMavlinkMessage(const mavlink_message_t &message);

public slots:
    void parameterChanged(int componentId, const QString &parameterName,
                          const QVariant &value);
    void parameterBatchSubmitted(int componentId, const QString &name,
                                 qulonglong batchId);
    void parameterWriteAcknowledged(int componentId, const QString &name,
                                    const QVariant &value, int type);
    void parameterWriteFailed(qulonglong transactionId,
                              qulonglong batchId, int componentId,
                              const QString &name, int reason,
                              const QString &message);
    void parameterWriteCancelled(qulonglong transactionId,
                                 qulonglong batchId, int componentId,
                                 const QString &name);
    void parameterBatchCompleted(qulonglong batchId,
                                 int succeeded, int failed);
    void parameterWriteSubmissionFailed(int componentId,
                                        const QString &name,
                                        const QString &reason);

signals:
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);

private:
    void configureMetadataEditors();
    void setComboValue(QComboBox *combo, const QVariant &value,
                       bool valid);
    void syncFields();
    void syncLiveValues();
    void syncState();
    void writeField(const QString &name, const QVariant &value);

    ConfigBatteryMonitoring2ViewModel *m_viewModel = nullptr;
    ParameterMetaDataCatalog m_catalog;
    QWidget *m_editorPanel = nullptr;
    QComboBox *m_monitor = nullptr;
    QDoubleSpinBox *m_capacity = nullptr;
    QComboBox *m_voltPin = nullptr;
    QComboBox *m_currPin = nullptr;
    QDoubleSpinBox *m_voltMult = nullptr;
    QDoubleSpinBox *m_ampPerVolt = nullptr;
    QDoubleSpinBox *m_ampOffset = nullptr;
    QLabel *m_ampPerVoltLabel = nullptr;
    QLabel *m_liveVoltage = nullptr;
    QLabel *m_liveCurrent = nullptr;
    QDoubleSpinBox *m_calculatedVoltMult = nullptr;
    QDoubleSpinBox *m_calculatedAmpPerVolt = nullptr;
    QDoubleSpinBox *m_measuredVoltage = nullptr;
    QDoubleSpinBox *m_measuredCurrent = nullptr;
    QPushButton *m_applyVoltage = nullptr;
    QPushButton *m_applyCurrent = nullptr;
    QCheckBox *m_alertOnLowBattery = nullptr;
    QLabel *m_status = nullptr;
};

#endif // CONFIGBATTERYMONITORING2VIEW_H
