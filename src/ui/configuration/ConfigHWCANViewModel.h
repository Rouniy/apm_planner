#ifndef CONFIGHWCANVIEWMODEL_H
#define CONFIGHWCANVIEWMODEL_H

#include "ParamField.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariant>

class ConfigHWCANViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString Title READ Title CONSTANT)
    Q_PROPERTY(QString Status READ Status NOTIFY statusChanged)
    Q_PROPERTY(bool IsConnected READ IsConnected NOTIFY stateChanged)
    Q_PROPERTY(bool VehicleArmed READ VehicleArmed NOTIFY stateChanged)
    Q_PROPERTY(bool Busy READ Busy NOTIFY stateChanged)
    Q_PROPERTY(bool CanEditCanEnable READ CanEditCanEnable
               NOTIFY stateChanged)
    Q_PROPERTY(bool CanIssueCommands READ CanIssueCommands
               NOTIFY stateChanged)
    Q_PROPERTY(bool FactoryResetArmed READ FactoryResetArmed
               WRITE SetFactoryResetArmed NOTIFY stateChanged)
    Q_PROPERTY(bool CanFactoryReset READ CanFactoryReset
               NOTIFY stateChanged)

public:
    explicit ConfigHWCANViewModel(QObject *parent = nullptr);

    static int WriteTimeoutMs() { return 5000; }
    static int CommandTimeoutMs() { return 5000; }
    static int PreflightUavcanCommand() { return 243; }
    static int PreflightStorageCommand() { return 245; }

    QString Title() const;
    QString Status() const { return m_status; }
    QList<ParamOption> CanEnableOptions() const { return m_options; }
    QList<ParamField> Fields() const { return m_fields; }
    QList<int> PhysicalPortIndexes() const;
    QList<int> DriverIndexes() const;
    QVariant SelectedCanEnable() const { return m_selectedCanEnable; }
    bool IsConnected() const { return m_connected; }
    bool VehicleArmed() const { return m_vehicleArmed; }
    bool Busy() const;
    bool CanEditCanEnable() const;
    bool CanIssueCommands() const;
    bool FactoryResetArmed() const { return m_factoryResetArmed; }
    bool CanFactoryReset() const;
    bool HasCanEnableParameter() const { return m_hasCanEnable; }
    bool HasModernFields() const;
    bool SnapshotReady() const { return m_snapshotReady; }
    bool HasPendingWrite() const { return m_parameterWritePending; }
    int PendingCommand() const { return m_pendingCommand; }
    bool CommandResultUncertain() const { return m_commandResultUncertain; }
    int ComponentId() const { return m_componentId; }

    void setCatalog(const ParameterMetaDataCatalog &catalog);
    void setParameterSnapshot(
        const QList<ConfigFriendlyParameterValue> &parameters,
        int preferredComponent = 1);
    void setConnected(bool connected);
    void setArmed(bool armed);

    bool SetSelectedCanEnable(const QVariant &value);
    bool setFieldValue(const QString &name, const QVariant &value);
    void SetFactoryResetArmed(bool armed);
    bool StartEnumeration();
    bool StopEnumeration();
    bool SaveConfig();
    bool FactoryReset();
    void commandConfirmationTimedOut();

public slots:
    void parameterChanged(int componentId, const QString &name,
                          const QVariant &value);
    void parameterWriteFailed(int componentId, const QString &name,
                              const QString &reason);
    void commandAckReceived(int componentId, int command, int result);
    void commandSendFailed(int componentId, int command,
                           const QString &reason);

signals:
    void optionsChanged();
    void structureChanged();
    void fieldChanged(const QString &name);
    void selectedCanEnableChanged();
    void statusChanged();
    void stateChanged();
    void writeRequested(int componentId, const QString &name,
                        const QVariant &value);
    void commandRequested(int componentId, int command,
                          float param1, float param2);

private:
    bool beginCommand(int command, float param1, float param2,
                      const QString &label, bool factoryReset = false);
    void rebuildOptions();
    void rebuildFields();
    ParamField makeField(const QString &name) const;
    ParamField *fieldForName(const QString &name);
    const ParamField *fieldForName(const QString &name) const;
    bool isRelevantParameter(const QString &name) const;
    void selectAuthoritativeValue(const QVariant &value);
    void finishParameterWrite(bool matched,
                              const QString &reason = QString(),
                              bool receivedEcho = false);
    void finishCommand(const QString &status);
    void scheduleCommandTimeout();
    void setStatus(const QString &status);
    QString normalizedName(const QString &name) const;

    ParameterMetaDataCatalog m_catalog;
    QList<ParamOption> m_options;
    QList<ParamField> m_fields;
    QHash<QString, QVariant> m_values;
    QVariant m_selectedCanEnable;
    QVariant m_authoritativeCanEnable;
    QVariant m_previousCanEnable;
    QVariant m_expectedCanEnable;
    QString m_pendingParameterName;
    QString m_status;
    QString m_pendingCommandLabel;
    int m_componentId = 1;
    int m_pendingCommand = 0;
    quint64 m_parameterGeneration = 0;
    quint64 m_commandGeneration = 0;
    bool m_connected = false;
    bool m_vehicleArmed = false;
    bool m_snapshotReady = false;
    bool m_hasCanEnable = false;
    bool m_parameterWritePending = false;
    bool m_factoryResetArmed = false;
    bool m_commandResultUncertain = false;
};

#endif // CONFIGHWCANVIEWMODEL_H
