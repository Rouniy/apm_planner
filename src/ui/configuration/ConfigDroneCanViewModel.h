#ifndef CONFIGDRONECANVIEWMODEL_H
#define CONFIGDRONECANVIEWMODEL_H

#include "comm/DroneCanGetNodeInfoClient.h"
#include "comm/DroneCanGetSetClient.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QTimer;

struct DroneCanNode
{
    int id = -1;
    QString name = QStringLiteral("—");
    QString mode;
    QString health;
    quint32 uptimeSeconds = 0;
    QString hardwareVersion = QStringLiteral("—");
    QString softwareVersion = QStringLiteral("—");
    QString softwareCrc = QStringLiteral("—");
    QString hardwareUid = QStringLiteral("—");
    quint16 vendorSpecificStatusCode = 0;
    qint64 lastSeenMs = 0;
    bool canFd = false;
};

struct DroneCanParameter
{
    quint16 index = 0;
    QString name;
    QString value;
    QString minimumValue;
    QString maximumValue;
    QString defaultValue;
};

class ConfigDroneCanViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString Title READ Title CONSTANT)
    Q_PROPERTY(QString Status READ Status NOTIFY statusChanged)
    Q_PROPERTY(QString NodeStatus READ NodeStatus NOTIFY nodeStatusChanged)
    Q_PROPERTY(QString ConnectLabel READ ConnectLabel NOTIFY stateChanged)
    Q_PROPERTY(bool IsConnected READ IsConnected NOTIFY stateChanged)
    Q_PROPERTY(bool VehicleConnected READ VehicleConnected NOTIFY stateChanged)
    Q_PROPERTY(bool VehicleArmed READ VehicleArmed NOTIFY stateChanged)
    Q_PROPERTY(bool IsBusy READ IsBusy NOTIFY stateChanged)
    Q_PROPERTY(bool IsReadingParameters READ IsReadingParameters
               NOTIFY stateChanged)
    Q_PROPERTY(int SelectedBusIndex READ SelectedBusIndex
               WRITE SetSelectedBusIndex NOTIFY stateChanged)

public:
    explicit ConfigDroneCanViewModel(QObject *parent = nullptr);

    static int CanForwardCommand() { return 32000; }
    static int NodeExpiryCheckPeriodMs() { return 1000; }

    QString Title() const;
    QString Status() const { return m_status; }
    QString NodeStatus() const { return m_nodeStatus; }
    QString ConnectLabel() const;
    QStringList BusOptions() const;
    QList<DroneCanNode> Nodes() const { return m_nodes; }
    QList<DroneCanParameter> Parameters() const { return m_parameters; }
    bool IsConnected() const { return m_connected; }
    bool VehicleConnected() const { return m_vehicleConnected; }
    bool VehicleArmed() const { return m_vehicleArmed; }
    bool IsBusy() const { return m_busy; }
    bool IsReadingParameters() const { return m_readingParameters; }
    bool CanToggleConnection() const;
    bool CanChangeInterface() const;
    bool CanGetParameters() const;
    bool CanFilterParameters() const;
    int SelectedBusIndex() const { return m_selectedBusIndex; }
    int SelectedNodeId() const { return m_selectedNodeId; }

    void setVehicleConnected(bool connected);
    void setVehicleArmed(bool armed);
    void SetSelectedBusIndex(int index);
    bool ToggleConnect();
    void Disconnect(const QString &status = QString());
    bool Refresh();
    void SelectNode(int nodeId);
    bool GetParameters();
    void observeCanFrame(int bus, quint32 id, const QByteArray &data,
                         bool canFd, qint64 nowMs);
    void observeNodeInfo(
        const DroneCanGetNodeInfoClient::NodeInfo &info);
    void nodeInfoRequestFailed(int nodeId, const QString &reason);
    void observeParameter(
        const DroneCanGetSetClient::Parameter &parameter);
    void parameterRequestFailed(int nodeId, const QString &reason);
    void parameterRequestCancelled(int nodeId, const QString &reason);
    void expireNodes(qint64 nowMs);
    void forwardingAckReceived(int result);
    void forwardingSendFailed(const QString &reason);

signals:
    void statusChanged();
    void nodeStatusChanged();
    void stateChanged();
    void nodesChanged();
    void parametersChanged();
    void selectionChanged();
    void canForwardingRequested(int componentId, int bus, bool enable);
    void nodeInfoRequested(int nodeId, bool preferCanFd);
    void parameterReadRequested(int nodeId, quint16 index,
                                bool preferCanFd);
    void parameterReadCancelRequested();
    void discoveryEpochReset();

private:
    void setStatus(const QString &status);
    void setNodeStatus(const QString &status);
    void cancelParameterRead(bool clearParameters);
    void clearParameters();
    bool requestNextParameter();
    static QString parameterValueText(
        const DroneCanGetSetClient::Value &value);
    static QString numericValueText(
        const DroneCanGetSetClient::NumericValue &value);
    static QString parameterNameText(const QByteArray &name);
    void expireOfflineNodes();
    static QString healthText(int health);
    static QString modeText(int mode);

    QTimer *m_nodeExpiryTimer = nullptr;
    QList<DroneCanNode> m_nodes;
    QList<DroneCanParameter> m_parameters;
    QString m_status;
    QString m_nodeStatus;
    int m_selectedBusIndex = 0;
    int m_selectedNodeId = -1;
    quint16 m_nextParameterIndex = 0;
    bool m_vehicleConnected = false;
    bool m_vehicleArmed = false;
    bool m_connected = false;
    bool m_busy = false;
    bool m_readingParameters = false;
};

#endif // CONFIGDRONECANVIEWMODEL_H
