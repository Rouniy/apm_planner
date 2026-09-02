#include "ConfigDroneCanViewModel.h"

#include "comm/DroneCanFrameCodec.h"

#include <QDateTime>
#include <QTimer>

#include <algorithm>

namespace {
constexpr int kAutopilotComponentId = 1;
constexpr int kAccepted = 0;
constexpr int kInProgress = 5;
}

ConfigDroneCanViewModel::ConfigDroneCanViewModel(QObject *parent)
    : QObject(parent),
      m_nodeExpiryTimer(new QTimer(this)),
      m_status(tr("Select MAVLink CAN1 or CAN2 to enumerate "
                  "DroneCAN / UAVCAN nodes.")),
      m_nodeStatus(tr("Select a node, then Get Parameters / Restart / "
                      "Update Firmware."))
{
    m_nodeExpiryTimer->setObjectName(
        QStringLiteral("droneCanNodeExpiryTimer"));
    m_nodeExpiryTimer->setInterval(NodeExpiryCheckPeriodMs());
    connect(m_nodeExpiryTimer, &QTimer::timeout,
            this, &ConfigDroneCanViewModel::expireOfflineNodes);
}

QString ConfigDroneCanViewModel::Title() const
{
    return tr("DroneCAN / UAVCAN");
}

QString ConfigDroneCanViewModel::ConnectLabel() const
{
    return m_connected ? tr("Disconnect") : tr("Connect");
}

QStringList ConfigDroneCanViewModel::BusOptions() const
{
    return {tr("MAVLink-CAN1"), tr("MAVLink-CAN2")};
}

bool ConfigDroneCanViewModel::CanToggleConnection() const
{
    return m_connected || (m_vehicleConnected && !m_busy);
}

bool ConfigDroneCanViewModel::CanChangeInterface() const
{
    return !m_connected && !m_busy;
}

bool ConfigDroneCanViewModel::CanGetParameters() const
{
    return m_connected && !m_busy && !m_readingParameters
        && m_selectedNodeId >= 1;
}

bool ConfigDroneCanViewModel::CanFilterParameters() const
{
    return m_connected && !m_readingParameters
        && m_selectedNodeId >= 1 && !m_parameters.isEmpty();
}

void ConfigDroneCanViewModel::setVehicleConnected(bool connected)
{
    if (m_vehicleConnected == connected) {
        return;
    }
    m_vehicleConnected = connected;
    if (!m_vehicleConnected) {
        Disconnect(tr("Not connected — open the MAVLink link first."));
        m_nodes.clear();
        m_selectedNodeId = -1;
        emit nodesChanged();
        emit selectionChanged();
    }
    emit stateChanged();
}

void ConfigDroneCanViewModel::setVehicleArmed(bool armed)
{
    if (m_vehicleArmed == armed) {
        return;
    }
    m_vehicleArmed = armed;
    if (m_vehicleArmed) {
        setNodeStatus(tr("Parameter writes, restart and firmware update "
                         "remain disabled while the vehicle is armed."));
    } else if (m_nodeStatus.contains(tr("while the vehicle is armed"))) {
        setNodeStatus(tr("Select a node, then Get Parameters / Restart / "
                         "Update Firmware."));
    }
    emit stateChanged();
}

void ConfigDroneCanViewModel::SetSelectedBusIndex(int index)
{
    if (index < 0 || index >= BusOptions().size()
        || m_selectedBusIndex == index || !CanChangeInterface()) {
        return;
    }
    m_selectedBusIndex = index;
    emit stateChanged();
}

bool ConfigDroneCanViewModel::ToggleConnect()
{
    if (m_connected) {
        Disconnect(tr("Disconnected."));
        return true;
    }
    if (!m_vehicleConnected) {
        setStatus(tr("Not connected — open the MAVLink link first."));
        return false;
    }
    if (m_busy) {
        return false;
    }

    m_connected = true;
    m_busy = true;
    m_nodes.clear();
    m_selectedNodeId = -1;
    emit nodesChanged();
    emit selectionChanged();
    setStatus(tr("Starting MAVLink CAN%1 forwarding…")
                  .arg(m_selectedBusIndex + 1));
    emit stateChanged();
    // Start before the synchronous request. A transport adapter may report
    // failure from inside the signal, and Disconnect() must be the final
    // authority that stops this timer.
    m_nodeExpiryTimer->start();
    emit canForwardingRequested(kAutopilotComponentId,
                                m_selectedBusIndex + 1, true);
    return m_connected;
}

void ConfigDroneCanViewModel::Disconnect(const QString &status)
{
    const bool wasConnected = m_connected;
    const bool wasReadingParameters = m_readingParameters;
    const bool hadParameters = !m_parameters.isEmpty();
    const int bus = m_selectedBusIndex + 1;
    cancelParameterRead(true);
    m_connected = false;
    m_busy = false;
    m_nodeExpiryTimer->stop();
    if (wasConnected) {
        emit canForwardingRequested(kAutopilotComponentId, bus, false);
    }
    if (!status.isNull()) {
        setStatus(status);
    }
    if (wasReadingParameters) {
        setNodeStatus(tr("DroneCAN parameter read cancelled because the "
                         "bus session was closed."));
    } else if (hadParameters) {
        setNodeStatus(tr("Reconnect and select a node to read parameters."));
    }
    emit stateChanged();
}

bool ConfigDroneCanViewModel::Refresh()
{
    if (!m_connected) {
        setStatus(tr("Connect first to refresh the node list."));
        return false;
    }
    if (m_busy) {
        setStatus(tr("Wait for MAVLink CAN forwarding confirmation."));
        return false;
    }
    cancelParameterRead(true);
    m_nodes.clear();
    m_selectedNodeId = -1;
    emit nodesChanged();
    emit selectionChanged();
    emit discoveryEpochReset();
    setStatus(tr("Waiting for fresh DroneCAN node status…"));
    setNodeStatus(tr("Select a node after fresh discovery completes."));
    emit stateChanged();
    return true;
}

void ConfigDroneCanViewModel::SelectNode(int nodeId)
{
    const auto found = std::find_if(
        m_nodes.constBegin(), m_nodes.constEnd(),
        [nodeId](const DroneCanNode &node) { return node.id == nodeId; });
    const int selected = found == m_nodes.constEnd() ? -1 : nodeId;
    if (m_selectedNodeId == selected) {
        return;
    }
    cancelParameterRead(true);
    m_selectedNodeId = selected;
    if (selected < 0) {
        setNodeStatus(tr("Select a node, then Get Parameters / Restart / "
                         "Update Firmware."));
    } else if (found->name == QStringLiteral("—")) {
        setNodeStatus(tr("Node %1 selected. Reading its GetNodeInfo "
                         "identity…")
                          .arg(selected));
    } else {
        setNodeStatus(
            tr("Node %1 identity loaded. Click Get Parameters to read its "
               "DroneCAN parameter list.")
                .arg(selected));
    }
    emit selectionChanged();
    emit stateChanged();
}

bool ConfigDroneCanViewModel::GetParameters()
{
    if (!CanGetParameters()) {
        if (!m_connected) {
            setNodeStatus(tr("Connect to a DroneCAN bus first."));
        } else if (m_selectedNodeId < 1) {
            setNodeStatus(tr("Select a DroneCAN node first."));
        }
        return false;
    }

    clearParameters();
    m_nextParameterIndex = 0;
    m_readingParameters = true;
    setNodeStatus(tr("Reading parameters from DroneCAN node %1…")
                      .arg(m_selectedNodeId));
    emit stateChanged();
    requestNextParameter();
    return m_readingParameters;
}

void ConfigDroneCanViewModel::observeCanFrame(
    int bus, quint32 id, const QByteArray &data, bool canFd, qint64 nowMs)
{
    if (!m_connected || bus != m_selectedBusIndex) {
        return;
    }
    DroneCanNodeStatusFrame status;
    if (!DroneCanFrameCodec::decodeNodeStatus(id, data, &status)) {
        return;
    }

    auto found = std::find_if(
        m_nodes.begin(), m_nodes.end(),
        [&status](const DroneCanNode &node) {
            return node.id == status.nodeId;
        });
    const bool discovered = found == m_nodes.end();
    const bool restarted = !discovered
        && status.uptimeSeconds < found->uptimeSeconds;
    if (discovered) {
        DroneCanNode node;
        node.id = status.nodeId;
        m_nodes.append(node);
        found = std::prev(m_nodes.end());
    }
    found->uptimeSeconds = status.uptimeSeconds;
    found->health = healthText(status.health);
    found->mode = modeText(status.mode);
    found->vendorSpecificStatusCode = status.vendorSpecificStatusCode;
    found->lastSeenMs = nowMs;
    found->canFd = canFd;
    if (restarted) {
        // A node ID may be reused after reboot. Do not keep presenting the
        // previous incarnation's identity while the new GetNodeInfo request
        // is pending or if it never answers.
        found->name = QStringLiteral("—");
        found->hardwareVersion = QStringLiteral("—");
        found->softwareVersion = QStringLiteral("—");
        found->softwareCrc = QStringLiteral("—");
        found->hardwareUid = QStringLiteral("—");
        if (m_selectedNodeId == status.nodeId) {
            cancelParameterRead(true);
            setNodeStatus(tr("Node %1 restarted. Reading its GetNodeInfo "
                             "identity…")
                              .arg(status.nodeId));
        }
    }
    std::sort(m_nodes.begin(), m_nodes.end(),
              [](const DroneCanNode &left, const DroneCanNode &right) {
        return left.id < right.id;
    });
    if (discovered) {
        setStatus(tr("Detected DroneCAN node %1 on CAN%2.")
                      .arg(status.nodeId)
                      .arg(m_selectedBusIndex + 1));
    }
    emit nodesChanged();
    if (discovered || restarted) {
        emit nodeInfoRequested(status.nodeId, canFd);
    }
}

void ConfigDroneCanViewModel::observeNodeInfo(
    const DroneCanGetNodeInfoClient::NodeInfo &info)
{
    if (!m_connected || info.busIndex != m_selectedBusIndex
        || info.nodeId < 1 || info.nodeId > 127) {
        return;
    }
    auto found = std::find_if(
        m_nodes.begin(), m_nodes.end(), [&info](const DroneCanNode &node) {
            return node.id == info.nodeId;
        });
    if (found == m_nodes.end()) {
        return;
    }

    found->name = info.name;
    found->uptimeSeconds = info.status.uptimeSeconds;
    found->health = healthText(info.status.health);
    found->mode = modeText(info.status.mode);
    found->vendorSpecificStatusCode =
        info.status.vendorSpecificStatusCode;
    found->hardwareVersion = QStringLiteral("%1.%2")
        .arg(info.hardwareVersion.major)
        .arg(info.hardwareVersion.minor);
    found->softwareVersion = QStringLiteral("%1.%2")
        .arg(info.softwareVersion.major)
        .arg(info.softwareVersion.minor);
    found->softwareCrc = info.softwareVersion.hasImageCrc()
        ? QStringLiteral("%1")
              .arg(static_cast<qulonglong>(
                       info.softwareVersion.imageCrc),
                   16, 16,
                   QLatin1Char('0'))
              .toUpper()
        : QStringLiteral("—");
    found->hardwareUid = QString::fromLatin1(
        info.hardwareVersion.uniqueId.toHex(' ').toUpper());
    found->canFd = info.canFd;
    setStatus(tr("Read identity for DroneCAN node %1 (%2).")
                  .arg(info.nodeId)
                  .arg(info.name));
    if (m_selectedNodeId == info.nodeId) {
        setNodeStatus(
            tr("Node %1 identity loaded. Click Get Parameters to read its "
               "DroneCAN parameter list.")
                .arg(info.nodeId));
    }
    emit nodesChanged();
}

void ConfigDroneCanViewModel::nodeInfoRequestFailed(
    int nodeId, const QString &reason)
{
    const auto found = std::find_if(
        m_nodes.constBegin(), m_nodes.constEnd(),
        [nodeId](const DroneCanNode &node) { return node.id == nodeId; });
    if (found == m_nodes.constEnd()) {
        return;
    }
    setStatus(tr("Unable to read DroneCAN node %1 identity: %2")
                  .arg(nodeId)
                  .arg(reason));
    if (m_selectedNodeId == nodeId) {
        setNodeStatus(tr("Node %1 did not answer GetNodeInfo. Refresh to "
                         "start a new discovery epoch.")
                          .arg(nodeId));
    }
}

void ConfigDroneCanViewModel::observeParameter(
    const DroneCanGetSetClient::Parameter &parameter)
{
    if (!m_readingParameters || !m_connected
        || parameter.busIndex != m_selectedBusIndex
        || parameter.nodeId != m_selectedNodeId
        || parameter.requestedIndex != m_nextParameterIndex
        || !parameter.requestedName.isEmpty()) {
        return;
    }

    if (!parameter.exists()) {
        m_readingParameters = false;
        setNodeStatus(
            tr("Loaded %1 DroneCAN parameter(s) from node %2.")
                .arg(m_parameters.size())
                .arg(m_selectedNodeId));
        emit stateChanged();
        return;
    }

    DroneCanParameter row;
    row.index = parameter.requestedIndex;
    row.name = parameterNameText(parameter.name);
    row.value = parameterValueText(parameter.value);
    row.minimumValue = numericValueText(parameter.minimumValue);
    row.maximumValue = numericValueText(parameter.maximumValue);
    row.defaultValue = parameterValueText(parameter.defaultValue);
    m_parameters.append(row);
    emit parametersChanged();

    if (m_nextParameterIndex == 8191U) {
        m_readingParameters = false;
        setNodeStatus(
            tr("Stopped after 8192 DroneCAN parameters from node %1; "
               "the uint13 index space is exhausted.")
                .arg(m_selectedNodeId));
        emit stateChanged();
        return;
    }

    ++m_nextParameterIndex;
    setNodeStatus(
        tr("Reading parameters from DroneCAN node %1… %2 loaded")
            .arg(m_selectedNodeId)
            .arg(m_parameters.size()));
    requestNextParameter();
}

void ConfigDroneCanViewModel::parameterRequestFailed(
    int nodeId, const QString &reason)
{
    if (!m_readingParameters || nodeId != m_selectedNodeId) {
        return;
    }
    m_readingParameters = false;
    setNodeStatus(
        tr("Unable to read DroneCAN parameters from node %1: %2")
            .arg(nodeId)
            .arg(reason));
    emit stateChanged();
}

void ConfigDroneCanViewModel::parameterRequestCancelled(
    int nodeId, const QString &reason)
{
    if (!m_readingParameters || nodeId != m_selectedNodeId) {
        return;
    }
    m_readingParameters = false;
    setNodeStatus(
        tr("DroneCAN parameter read for node %1 was cancelled: %2")
            .arg(nodeId)
            .arg(reason));
    emit stateChanged();
}

void ConfigDroneCanViewModel::expireNodes(qint64 nowMs)
{
    const int previousSize = m_nodes.size();
    m_nodes.erase(
        std::remove_if(m_nodes.begin(), m_nodes.end(),
                       [nowMs](const DroneCanNode &node) {
            return nowMs - node.lastSeenMs
                > DroneCanFrameCodec::NodeOfflineTimeoutMs;
        }),
        m_nodes.end());
    if (m_nodes.size() == previousSize) {
        return;
    }
    if (m_selectedNodeId >= 1
        && std::none_of(m_nodes.constBegin(), m_nodes.constEnd(),
                        [this](const DroneCanNode &node) {
            return node.id == m_selectedNodeId;
        })) {
        cancelParameterRead(true);
        m_selectedNodeId = -1;
        setNodeStatus(tr("The selected DroneCAN node went offline."));
        emit selectionChanged();
        emit stateChanged();
    }
    emit nodesChanged();
}

void ConfigDroneCanViewModel::forwardingAckReceived(int result)
{
    if (!m_connected) {
        return;
    }
    if (result == kInProgress) {
        m_busy = true;
        setStatus(tr("MAVLink CAN forwarding is starting…"));
        emit stateChanged();
        return;
    }
    if (result == kAccepted) {
        m_busy = false;
        setStatus(tr("Listening for DroneCAN nodes on MAVLink CAN%1.")
                      .arg(m_selectedBusIndex + 1));
        emit stateChanged();
        return;
    }
    const QString failure = tr("MAVLink CAN forwarding was rejected "
                               "(MAV_RESULT %1).").arg(result);
    Disconnect(failure);
}

void ConfigDroneCanViewModel::forwardingSendFailed(const QString &reason)
{
    if (!m_connected) {
        return;
    }
    Disconnect(reason.isEmpty()
                   ? tr("Unable to send MAVLink CAN forwarding request.")
                   : tr("Unable to start MAVLink CAN forwarding: %1")
                         .arg(reason));
}

void ConfigDroneCanViewModel::cancelParameterRead(bool clearParametersFlag)
{
    const bool wasReading = m_readingParameters;
    const bool hadParameters = !m_parameters.isEmpty();
    m_readingParameters = false;
    m_nextParameterIndex = 0;
    if (clearParametersFlag) {
        clearParameters();
    }
    if (wasReading) {
        emit parameterReadCancelRequested();
    }
    if (wasReading || (clearParametersFlag && hadParameters)) {
        emit stateChanged();
    }
}

void ConfigDroneCanViewModel::clearParameters()
{
    if (m_parameters.isEmpty()) {
        return;
    }
    m_parameters.clear();
    emit parametersChanged();
}

bool ConfigDroneCanViewModel::requestNextParameter()
{
    if (!m_readingParameters) {
        return false;
    }
    const auto found = std::find_if(
        m_nodes.constBegin(), m_nodes.constEnd(),
        [this](const DroneCanNode &node) {
            return node.id == m_selectedNodeId;
        });
    if (found == m_nodes.constEnd()) {
        cancelParameterRead(true);
        setNodeStatus(tr("The selected DroneCAN node is no longer online."));
        return false;
    }
    emit parameterReadRequested(found->id, m_nextParameterIndex,
                                found->canFd);
    return m_readingParameters;
}

QString ConfigDroneCanViewModel::parameterValueText(
    const DroneCanGetSetClient::Value &value)
{
    switch (value.type) {
    case DroneCanGetSetClient::Value::Empty:
        return QStringLiteral("—");
    case DroneCanGetSetClient::Value::Integer:
        return QString::number(value.integerValue);
    case DroneCanGetSetClient::Value::Real:
        return QString::number(double(value.realValue), 'g', 9);
    case DroneCanGetSetClient::Value::Boolean:
        return value.booleanValue ? tr("True") : tr("False");
    case DroneCanGetSetClient::Value::String:
        return parameterNameText(value.stringValue);
    }
    return QStringLiteral("—");
}

QString ConfigDroneCanViewModel::numericValueText(
    const DroneCanGetSetClient::NumericValue &value)
{
    switch (value.type) {
    case DroneCanGetSetClient::NumericValue::Empty:
        return QStringLiteral("—");
    case DroneCanGetSetClient::NumericValue::Integer:
        return QString::number(value.integerValue);
    case DroneCanGetSetClient::NumericValue::Real:
        return QString::number(double(value.realValue), 'g', 9);
    }
    return QStringLiteral("—");
}

QString ConfigDroneCanViewModel::parameterNameText(const QByteArray &name)
{
    const QString decoded = QString::fromUtf8(name.constData(), name.size());
    bool printable = decoded.toUtf8() == name;
    for (QChar character : decoded) {
        if (!character.isPrint()) {
            printable = false;
            break;
        }
    }
    return printable
        ? decoded
        : QStringLiteral("0x%1")
              .arg(QString::fromLatin1(name.toHex().toUpper()));
}

void ConfigDroneCanViewModel::setStatus(const QString &status)
{
    if (m_status == status) {
        return;
    }
    m_status = status;
    emit statusChanged();
}

void ConfigDroneCanViewModel::setNodeStatus(const QString &status)
{
    if (m_nodeStatus == status) {
        return;
    }
    m_nodeStatus = status;
    emit nodeStatusChanged();
}

void ConfigDroneCanViewModel::expireOfflineNodes()
{
    if (m_connected && m_vehicleConnected) {
        expireNodes(QDateTime::currentMSecsSinceEpoch());
    }
}

QString ConfigDroneCanViewModel::healthText(int health)
{
    switch (health) {
    case 0: return tr("OK");
    case 1: return tr("WARNING");
    case 2: return tr("ERROR");
    case 3: return tr("CRITICAL");
    default: return tr("UNKNOWN");
    }
}

QString ConfigDroneCanViewModel::modeText(int mode)
{
    switch (mode) {
    case 0: return tr("OPERATIONAL");
    case 1: return tr("INITIALIZATION");
    case 2: return tr("MAINTENANCE");
    case 3: return tr("SOFTWARE_UPDATE");
    case 7: return tr("OFFLINE");
    default: return tr("UNKNOWN");
    }
}
