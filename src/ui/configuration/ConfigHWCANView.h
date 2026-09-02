#ifndef CONFIGHWCANVIEW_H
#define CONFIGHWCANVIEW_H

#include "ConfigHWCANViewModel.h"

#include <QHash>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

class ConfigHWCANView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigHWCANView(
        const ParameterMetaDataCatalog &catalog = ParameterMetaDataCatalog(),
        QWidget *parent = nullptr);
    ~ConfigHWCANView() override;

    QSize sizeHint() const override;
    ConfigHWCANViewModel *viewModel() const { return m_viewModel; }

    void setCatalog(const ParameterMetaDataCatalog &catalog);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setConnected(bool connected);
    void setArmed(bool armed);

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QString &reason);
    void commandAckReceived(int componentId, int command, int result);
    void commandSendFailed(int componentId, int command,
                           const QString &reason);

signals:
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);
    void commandRequested(int componentId, int command,
                          float param1, float param2);

private:
    struct FieldWidgets;

    void rebuildOptions();
    void rebuildFields();
    void syncField(const QString &name);
    void syncFromModel();
    void clearFields();

    ConfigHWCANViewModel *m_viewModel = nullptr;
    QWidget *m_canEnableRow = nullptr;
    QComboBox *m_canEnableCombo = nullptr;
    QGroupBox *m_portsGroup = nullptr;
    QGroupBox *m_driversGroup = nullptr;
    QLabel *m_restartNote = nullptr;
    QVBoxLayout *m_portsLayout = nullptr;
    QVBoxLayout *m_driversLayout = nullptr;
    QPushButton *m_startEnumerationButton = nullptr;
    QPushButton *m_stopEnumerationButton = nullptr;
    QPushButton *m_saveConfigButton = nullptr;
    QCheckBox *m_factoryResetCheckBox = nullptr;
    QPushButton *m_factoryResetButton = nullptr;
    QLabel *m_statusLabel = nullptr;
    QHash<QString, FieldWidgets *> m_fields;
};

#endif // CONFIGHWCANVIEW_H
