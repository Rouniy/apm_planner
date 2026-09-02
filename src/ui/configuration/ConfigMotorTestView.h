#ifndef CONFIGMOTORTESTVIEW_H
#define CONFIGMOTORTESTVIEW_H

#include "ConfigMotorTestViewModel.h"

#include <QHash>
#include <QWidget>

class QLabel;
class QHideEvent;
class QPushButton;
class QSpinBox;
class QVBoxLayout;

class ConfigMotorTestView final : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigMotorTestView(
        const ParameterMetaDataCatalog &catalog = ParameterMetaDataCatalog(),
        QWidget *parent = nullptr);

    static QString SafetyConfirmationTitle();
    static QString SafetyConfirmationText();

    QSize sizeHint() const override;
    void setCatalog(const ParameterMetaDataCatalog &catalog);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setMotorLayoutJson(const QByteArray &json);
    void setVehicleType(int mavType);
    void setConnected(bool connected);
    void setArmed(bool armed);
    ConfigMotorTestViewModel *viewModel() const { return m_viewModel; }

public slots:
    void commandAckReceived(int componentId, int command, int result);
    void commandSendFailed(int componentId, const QString &reason);
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QString &reason);

signals:
    void motorTestRequested(int componentId, int motor,
                            int throttleType, int throttle,
                            int durationSec, int motorCount,
                            int testOrder);
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);

protected:
    void hideEvent(QHideEvent *event) override;

private:
    bool confirmMotorTest();
    void rebuildMotors();
    void syncSettings();
    void syncState();

    ConfigMotorTestViewModel *m_viewModel = nullptr;
    QLabel *m_frameClassLabel = nullptr;
    QLabel *m_frameTypeLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QSpinBox *m_throttleEditor = nullptr;
    QSpinBox *m_durationEditor = nullptr;
    QVBoxLayout *m_motorsLayout = nullptr;
    QWidget *m_sequenceRow = nullptr;
    QPushButton *m_setSpinArmButton = nullptr;
    QPushButton *m_setSpinMinButton = nullptr;
    QPushButton *m_testAllButton = nullptr;
    QPushButton *m_stopAllButton = nullptr;
    QHash<int, QPushButton *> m_motorButtons;
};

#endif
