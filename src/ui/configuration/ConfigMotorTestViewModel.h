#ifndef CONFIGMOTORTESTVIEWMODEL_H
#define CONFIGMOTORTESTVIEWMODEL_H

#include "ParamField.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariant>

struct MotorTestItem
{
    int TestOrder = 0;
    int MotorNumber = 0;
    QString Label;
    QString Rotation;
};

class ConfigMotorTestViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString Title READ Title CONSTANT)
    Q_PROPERTY(QString Instructions READ Instructions CONSTANT)
    Q_PROPERTY(QString FrameClass READ FrameClass NOTIFY motorsChanged)
    Q_PROPERTY(QString FrameType READ FrameType NOTIFY motorsChanged)
    Q_PROPERTY(int ThrottlePercent READ ThrottlePercent
               WRITE setThrottlePercent NOTIFY settingsChanged)
    Q_PROPERTY(int DurationSec READ DurationSec
               WRITE setDurationSec NOTIFY settingsChanged)
    Q_PROPERTY(QString Status READ Status NOTIFY statusChanged)
    Q_PROPERTY(bool Busy READ Busy NOTIFY stateChanged)
    Q_PROPERTY(bool CanRun READ CanRun NOTIFY stateChanged)

public:
    explicit ConfigMotorTestViewModel(QObject *parent = nullptr);

    QString Title() const;
    QString Instructions() const;
    QString FrameClass() const { return m_frameClass; }
    QString FrameType() const { return m_frameType; }
    int ThrottlePercent() const { return m_throttlePercent; }
    int DurationSec() const { return m_durationSec; }
    QString Status() const { return m_status; }
    bool Busy() const;
    bool CanRun() const;
    bool Connected() const { return m_connected; }
    bool Armed() const { return m_armed; }
    bool TestMayBeActive() const { return m_testMayBeActive; }
    bool NeedsEmergencyStop() const {
        return m_testMayBeActive || m_commandAckUncertain;
    }
    bool ShouldAutoStop() const {
        return NeedsEmergencyStop() && !m_stopAttempted;
    }
    int ComponentId() const { return m_componentId; }
    QList<MotorTestItem> Motors() const { return m_motors; }

    void setCatalog(const ParameterMetaDataCatalog &catalog);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setMotorLayoutJson(const QByteArray &json);
    void setVehicleType(int mavType);
    void setConnected(bool connected);
    void setArmed(bool armed);
    void setThrottlePercent(int percent);
    void setDurationSec(int seconds);

    bool TestMotor(int testOrder, bool confirmed);
    bool TestAllSequence(bool confirmed);
    bool StopAll();
    bool SetSpinArm();
    bool SetSpinMin();

public slots:
    void commandAckReceived(int componentId, int command, int result);
    void commandSendFailed(int componentId, const QString &reason);
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QString &reason);

signals:
    void motorsChanged();
    void settingsChanged();
    void stateChanged();
    void statusChanged();
    void motorTestRequested(int componentId, int motor,
                            int throttleType, int throttle,
                            int durationSec, int motorCount,
                            int testOrder);
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);

private:
    struct LayoutMotor
    {
        int number = 0;
        int testOrder = 0;
        QString rotation;
    };

    struct MotorCommand
    {
        int motor = 0;
        int throttle = 0;
        int durationSec = 0;
        int motorCount = 0;
        int testOrder = 0;
    };

    struct PendingParameterWrite
    {
        QString name;
        QVariant expectedValue;
        quint64 generation = 0;
    };

    void rebuildMotors();
    bool tryFrameLayout(const QString &classParameter,
                        const QString &typeParameter,
                        QList<LayoutMotor> *layout);
    QList<LayoutMotor> lookupLayout(int frameClass, int frameType) const;
    QString optionLabel(const QString &name, const QVariant &value) const;
    int fallbackMotorCount() const;
    bool beginMotorCommands(const QList<MotorCommand> &commands,
                            bool confirmed, bool stopBatch);
    bool requireParameter(const QString &name);
    void queueParameterWrite(const QString &name, double value);
    void finishParameterWrite(bool matched, const QString &reason = QString());
    void finishMotorCommands(const QString &status);
    void dispatchNextMotorCommand();
    void maybeDispatchNextMotorCommand();
    int motorCommandDurationMs(const MotorCommand &command) const;
    void finishStop(const QString &status);
    void scheduleCommandTimeout();
    void scheduleActivityExpiry(int delayMs);
    void cancelPending(const QString &status);
    void setStatus(const QString &status);
    QString normalizedName(const QString &name) const;

    ParameterMetaDataCatalog m_catalog;
    QByteArray m_layoutJson;
    QHash<QString, QVariant> m_values;
    QList<MotorTestItem> m_motors;
    QString m_frameClass;
    QString m_frameType;
    QString m_status;
    QList<MotorCommand> m_pendingCommands;
    PendingParameterWrite m_pendingParameter;
    int m_componentId = 1;
    int m_vehicleType = 0;
    int m_throttlePercent = 8;
    int m_durationSec = 2;
    int m_expectedCommandAcks = 0;
    int m_receivedCommandAcks = 0;
    int m_testActivityDurationMs = 0;
    quint64 m_commandGeneration = 0;
    quint64 m_parameterGeneration = 0;
    quint64 m_testActivityGeneration = 0;
    bool m_connected = false;
    bool m_armed = false;
    bool m_stopBatch = false;
    bool m_testMayBeActive = false;
    bool m_commandAckUncertain = false;
    bool m_stopAckAmbiguous = false;
    bool m_stopSawArmed = false;
    bool m_stopAttempted = false;
    bool m_waitingForSequenceDisarm = false;
    bool m_sequenceDelayElapsed = false;
    bool m_motorTestArmedObserved = false;
    bool m_motorTestDisarmedAfterArmObserved = false;
};

Q_DECLARE_METATYPE(MotorTestItem)

#endif
