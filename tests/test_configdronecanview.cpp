#include <QtTest>

#include "comm/DroneCanFrameCodec.h"
#include "ui/configuration/ConfigDroneCanView.h"

#include <QApplication>
#include <QDateTime>
#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTimer>

namespace {
quint32 nodeStatusCanId(int nodeId, int dataTypeId = 341,
                        bool extended = true, bool service = false)
{
    quint32 id = (31U << 24U)
        | (quint32(dataTypeId) << 8U)
        | quint32(nodeId & 0x7F);
    if (service) {
        id |= 0x80U;
    }
    if (extended) {
        id |= DroneCanFrameCodec::ExtendedFrameFlag;
    }
    return id;
}

QByteArray nodeStatusPayload(quint32 uptime, int health, int mode,
                             int subMode, quint16 vendor,
                             quint8 transferId = 3)
{
    QByteArray data(8, '\0');
    data[0] = char(uptime & 0xFFU);
    data[1] = char((uptime >> 8U) & 0xFFU);
    data[2] = char((uptime >> 16U) & 0xFFU);
    data[3] = char((uptime >> 24U) & 0xFFU);
    data[4] = char(((health & 0x03) << 6)
                   | ((mode & 0x07) << 3)
                   | (subMode & 0x07));
    data[5] = char(vendor & 0xFFU);
    data[6] = char((vendor >> 8U) & 0xFFU);
    data[7] = char(0xC0U | (transferId & 0x1FU));
    return data;
}
} // namespace

class ConfigDroneCanViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void decodesStrictSingleFrameNodeStatus();
    void connectionAndRefreshUseMavlinkCanContract();
    void discoversUpdatesSortsAndExpiresNodes();
    void nodeInfoPopulatesMissionPlannerIdentityColumns();
    void nodeExpiryTimerNeverRequestsForwarding();
    void synchronousForwardingFailureLeavesExpiryTimerStopped();
    void parameterEnumerationRunsSequentiallyUntilNotFound();
    void parameterEnumerationCancelsOnContextChangesAndReportsErrors();
    void widgetMatchesMissionPlannerDiscoverySurface();
};

void ConfigDroneCanViewTest::decodesStrictSingleFrameNodeStatus()
{
    DroneCanNodeStatusFrame status;
    const QByteArray payload = nodeStatusPayload(
        0x78563412U, 1, 2, 5, 0xBEEF, 17);
    QVERIFY(DroneCanFrameCodec::decodeNodeStatus(
        nodeStatusCanId(42), payload, &status));
    QCOMPARE(status.nodeId, 42);
    QCOMPARE(status.uptimeSeconds, quint32(0x78563412U));
    QCOMPARE(status.health, quint8(1));
    QCOMPARE(status.mode, quint8(2));
    QCOMPARE(status.subMode, quint8(5));
    QCOMPARE(status.vendorSpecificStatusCode, quint16(0xBEEF));
    QCOMPARE(status.transferId, quint8(17));

    QVERIFY(!DroneCanFrameCodec::decodeNodeStatus(
        nodeStatusCanId(42, 341, false), payload, &status));
    QVERIFY(!DroneCanFrameCodec::decodeNodeStatus(
        nodeStatusCanId(42, 342), payload, &status));
    QVERIFY(!DroneCanFrameCodec::decodeNodeStatus(
        nodeStatusCanId(42, 1, true, true), payload, &status));
    QVERIFY(!DroneCanFrameCodec::decodeNodeStatus(
        nodeStatusCanId(0), payload, &status));
    QVERIFY(!DroneCanFrameCodec::decodeNodeStatus(
        nodeStatusCanId(42), payload.left(7), &status));
    QVERIFY(!DroneCanFrameCodec::decodeNodeStatus(
        nodeStatusCanId(42), payload + QByteArray(1, '\0'), &status));
    QByteArray multiFrame = payload;
    multiFrame[7] = char(0x80U);
    QVERIFY(!DroneCanFrameCodec::decodeNodeStatus(
        nodeStatusCanId(42), multiFrame, &status));
}

void ConfigDroneCanViewTest::connectionAndRefreshUseMavlinkCanContract()
{
    ConfigDroneCanViewModel model;
    QSignalSpy forwarding(
        &model, &ConfigDroneCanViewModel::canForwardingRequested);
    QSignalSpy discoveryReset(
        &model, &ConfigDroneCanViewModel::discoveryEpochReset);

    QCOMPARE(model.BusOptions(),
             QStringList({QStringLiteral("MAVLink-CAN1"),
                          QStringLiteral("MAVLink-CAN2")}));
    QCOMPARE(ConfigDroneCanViewModel::CanForwardCommand(), 32000);
    QVERIFY(!model.ToggleConnect());
    QVERIFY(model.Status().contains(QStringLiteral("Not connected")));

    model.setVehicleConnected(true);
    model.SetSelectedBusIndex(1);
    QVERIFY(model.ToggleConnect());
    QCOMPARE(forwarding.count(), 1);
    QCOMPARE(forwarding.first().at(0).toInt(), 1);
    QCOMPARE(forwarding.first().at(1).toInt(), 2);
    QVERIFY(forwarding.first().at(2).toBool());
    QVERIFY(model.IsConnected());
    QVERIFY(model.IsBusy());

    model.forwardingAckReceived(5);
    QVERIFY(model.IsBusy());
    model.forwardingAckReceived(0);
    QVERIFY(!model.IsBusy());
    QVERIFY(model.Status().contains(QStringLiteral("CAN2")));
    QVERIFY(model.Refresh());
    QCOMPARE(discoveryReset.count(), 1);
    // The application-scoped broker owns the one-second forwarding
    // keepalive; a model refresh only clears stale UI discovery state.
    QCOMPARE(forwarding.count(), 1);

    QVERIFY(model.ToggleConnect());
    QVERIFY(!model.IsConnected());
    QCOMPARE(forwarding.count(), 2);
    QVERIFY(!forwarding.last().at(2).toBool());

    QVERIFY(model.ToggleConnect());
    model.forwardingSendFailed(QStringLiteral("link lost"));
    QVERIFY(!model.IsConnected());
    QVERIFY(model.Status().contains(QStringLiteral("link lost")));
    QCOMPARE(forwarding.count(), 4);
    QVERIFY(!forwarding.last().at(2).toBool());
}

void ConfigDroneCanViewTest::discoversUpdatesSortsAndExpiresNodes()
{
    ConfigDroneCanViewModel model;
    QSignalSpy nodeInfoRequests(
        &model, &ConfigDroneCanViewModel::nodeInfoRequested);
    model.setVehicleConnected(true);
    QVERIFY(model.ToggleConnect());
    model.forwardingAckReceived(0);

    model.observeCanFrame(1, nodeStatusCanId(9),
                          nodeStatusPayload(10, 0, 0, 0, 1),
                          false, 1000);
    QVERIFY(model.Nodes().isEmpty());
    model.observeCanFrame(0, nodeStatusCanId(42),
                          nodeStatusPayload(20, 1, 2, 0, 8),
                          false, 1000);
    model.observeCanFrame(0, nodeStatusCanId(7),
                          nodeStatusPayload(30, 0, 0, 0, 2),
                          false, 1100);
    QCOMPARE(model.Nodes().size(), 2);
    QCOMPARE(model.Nodes().at(0).id, 7);
    QCOMPARE(model.Nodes().at(1).id, 42);
    QCOMPARE(model.Nodes().at(1).mode, QStringLiteral("MAINTENANCE"));
    QCOMPARE(model.Nodes().at(1).health, QStringLiteral("WARNING"));
    QCOMPARE(model.Nodes().at(1).vendorSpecificStatusCode, quint16(8));
    QCOMPARE(nodeInfoRequests.count(), 2);

    model.observeCanFrame(0, nodeStatusCanId(42),
                          nodeStatusPayload(21, 2, 3, 0, 9),
                          true, 2500);
    QCOMPARE(model.Nodes().size(), 2);
    QCOMPARE(model.Nodes().at(1).uptimeSeconds, quint32(21));
    QCOMPARE(model.Nodes().at(1).mode,
             QStringLiteral("SOFTWARE_UPDATE"));
    QCOMPARE(model.Nodes().at(1).health, QStringLiteral("ERROR"));
    QCOMPARE(nodeInfoRequests.count(), 2);
    model.observeCanFrame(0, nodeStatusCanId(42),
                          nodeStatusPayload(1, 0, 0, 0, 0),
                          false, 2600);
    QCOMPARE(nodeInfoRequests.count(), 3);

    model.SelectNode(7);
    QCOMPARE(model.SelectedNodeId(), 7);
    model.expireNodes(4101);
    QCOMPARE(model.Nodes().size(), 1);
    QCOMPARE(model.Nodes().first().id, 42);
    QCOMPARE(model.SelectedNodeId(), -1);

    model.setVehicleConnected(false);
    QVERIFY(model.Nodes().isEmpty());
    const QString disconnectedStatus = model.Status();
    model.observeCanFrame(0, nodeStatusCanId(50),
                          nodeStatusPayload(1, 0, 0, 0, 0),
                          false, 5000);
    QVERIFY(model.Nodes().isEmpty());
    QCOMPARE(model.Status(), disconnectedStatus);
}

void ConfigDroneCanViewTest::nodeInfoPopulatesMissionPlannerIdentityColumns()
{
    ConfigDroneCanViewModel model;
    model.setVehicleConnected(true);
    QVERIFY(model.ToggleConnect());
    model.forwardingAckReceived(0);
    model.observeCanFrame(0, nodeStatusCanId(42),
                          nodeStatusPayload(20, 1, 2, 0, 8),
                          false, 1000);

    DroneCanGetNodeInfoClient::NodeInfo info;
    info.brokerGeneration = 7;
    info.busIndex = 0;
    info.nodeId = 42;
    info.name = QStringLiteral("org.test");
    info.status.uptimeSeconds = 21;
    info.status.health = 0;
    info.status.mode = 0;
    info.hardwareVersion.major = 3;
    info.hardwareVersion.minor = 4;
    info.hardwareVersion.uniqueId = QByteArray::fromHex(
        "000102030405060708090a0b0c0d0e0f");
    info.softwareVersion.major = 1;
    info.softwareVersion.minor = 2;
    info.softwareVersion.optionalFieldFlags = 2;
    info.softwareVersion.imageCrc =
        Q_UINT64_C(0x0102030405060708);
    model.observeNodeInfo(info);

    QCOMPARE(model.Nodes().size(), 1);
    const DroneCanNode node = model.Nodes().first();
    QCOMPARE(node.name, QStringLiteral("org.test"));
    QCOMPARE(node.hardwareVersion, QStringLiteral("3.4"));
    QCOMPARE(node.softwareVersion, QStringLiteral("1.2"));
    QCOMPARE(node.softwareCrc,
             QStringLiteral("0102030405060708"));
    QCOMPARE(node.hardwareUid,
             QStringLiteral("00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F"));
    QCOMPARE(node.mode, QStringLiteral("OPERATIONAL"));
    QCOMPARE(node.health, QStringLiteral("OK"));

    model.observeCanFrame(0, nodeStatusCanId(42),
                          nodeStatusPayload(1, 0, 0, 0, 0),
                          false, 2000);
    const DroneCanNode restarted = model.Nodes().first();
    QCOMPARE(restarted.name, QStringLiteral("—"));
    QCOMPARE(restarted.hardwareVersion, QStringLiteral("—"));
    QCOMPARE(restarted.softwareVersion, QStringLiteral("—"));
    QCOMPARE(restarted.softwareCrc, QStringLiteral("—"));
    QCOMPARE(restarted.hardwareUid, QStringLiteral("—"));
}

void ConfigDroneCanViewTest::nodeExpiryTimerNeverRequestsForwarding()
{
    ConfigDroneCanViewModel model;
    QSignalSpy forwarding(
        &model, &ConfigDroneCanViewModel::canForwardingRequested);
    model.setVehicleConnected(true);
    QVERIFY(model.ToggleConnect());
    model.forwardingAckReceived(0);

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    model.observeCanFrame(
        0, nodeStatusCanId(12), nodeStatusPayload(1, 0, 0, 0, 0),
        false, nowMs - DroneCanFrameCodec::NodeOfflineTimeoutMs - 1);
    QCOMPARE(model.Nodes().size(), 1);
    auto *timer = model.findChild<QTimer *>(
        QStringLiteral("droneCanNodeExpiryTimer"));
    QVERIFY(timer);
    QVERIFY(QMetaObject::invokeMethod(timer, "timeout",
                                      Qt::DirectConnection));
    QVERIFY(model.Nodes().isEmpty());
    QCOMPARE(forwarding.count(), 1);
}

void ConfigDroneCanViewTest::synchronousForwardingFailureLeavesExpiryTimerStopped()
{
    ConfigDroneCanViewModel model;
    model.setVehicleConnected(true);
    connect(&model, &ConfigDroneCanViewModel::canForwardingRequested,
            &model, [&model](int, int, bool enable) {
        if (enable) {
            model.forwardingSendFailed(QStringLiteral("link lost"));
        }
    });

    QVERIFY(!model.ToggleConnect());
    QVERIFY(!model.IsConnected());
    auto *timer = model.findChild<QTimer *>(
        QStringLiteral("droneCanNodeExpiryTimer"));
    QVERIFY(timer);
    QVERIFY(!timer->isActive());
}

void ConfigDroneCanViewTest::parameterEnumerationRunsSequentiallyUntilNotFound()
{
    ConfigDroneCanViewModel model;
    QSignalSpy requests(
        &model, &ConfigDroneCanViewModel::parameterReadRequested);
    model.setVehicleConnected(true);
    QVERIFY(model.ToggleConnect());
    model.forwardingAckReceived(0);
    model.observeCanFrame(0, nodeStatusCanId(42),
                          nodeStatusPayload(20, 0, 0, 0, 0),
                          true, 1000);
    model.SelectNode(42);
    QVERIFY(model.CanGetParameters());
    QVERIFY(model.GetParameters());
    QVERIFY(model.IsReadingParameters());
    QCOMPARE(requests.count(), 1);
    QCOMPARE(requests.first().at(0).toInt(), 42);
    QCOMPARE(requests.first().at(1).toUInt(), quint32(0));
    QVERIFY(requests.first().at(2).toBool());

    DroneCanGetSetClient::Parameter first;
    first.busIndex = 0;
    first.nodeId = 42;
    first.requestedIndex = 0;
    first.name = QByteArray("CAN_NODE");
    first.value = DroneCanGetSetClient::Value::fromInteger(42);
    first.defaultValue = DroneCanGetSetClient::Value::fromInteger(10);
    first.minimumValue.type =
        DroneCanGetSetClient::NumericValue::Integer;
    first.minimumValue.integerValue = 1;
    first.maximumValue.type =
        DroneCanGetSetClient::NumericValue::Integer;
    first.maximumValue.integerValue = 127;
    model.observeParameter(first);
    QCOMPARE(model.Parameters().size(), 1);
    QCOMPARE(model.Parameters().first().index, quint16(0));
    QCOMPARE(model.Parameters().first().name, QStringLiteral("CAN_NODE"));
    QCOMPARE(model.Parameters().first().value, QStringLiteral("42"));
    QCOMPARE(model.Parameters().first().minimumValue, QStringLiteral("1"));
    QCOMPARE(model.Parameters().first().maximumValue,
             QStringLiteral("127"));
    QCOMPARE(model.Parameters().first().defaultValue,
             QStringLiteral("10"));
    QCOMPARE(requests.count(), 2);
    QCOMPARE(requests.last().at(1).toUInt(), quint32(1));

    DroneCanGetSetClient::Parameter second;
    second.busIndex = 0;
    second.nodeId = 42;
    second.requestedIndex = 1;
    second.name = QByteArray("GAIN");
    second.value = DroneCanGetSetClient::Value::fromReal(1.25F);
    second.defaultValue = DroneCanGetSetClient::Value::fromReal(0.5F);
    model.observeParameter(second);
    QCOMPARE(model.Parameters().size(), 2);
    QCOMPARE(requests.count(), 3);
    QCOMPARE(requests.last().at(1).toUInt(), quint32(2));

    DroneCanGetSetClient::Parameter end;
    end.busIndex = 0;
    end.nodeId = 42;
    end.requestedIndex = 2;
    model.observeParameter(end);
    QVERIFY(!model.IsReadingParameters());
    QVERIFY(model.CanGetParameters());
    QVERIFY(model.CanFilterParameters());
    QCOMPARE(requests.count(), 3);
    QVERIFY(model.NodeStatus().contains(QStringLiteral("Loaded 2")));
}

void ConfigDroneCanViewTest::parameterEnumerationCancelsOnContextChangesAndReportsErrors()
{
    ConfigDroneCanViewModel model;
    QSignalSpy cancellations(
        &model, &ConfigDroneCanViewModel::parameterReadCancelRequested);
    model.setVehicleConnected(true);
    QVERIFY(model.ToggleConnect());
    model.forwardingAckReceived(0);
    model.observeCanFrame(0, nodeStatusCanId(42),
                          nodeStatusPayload(20, 0, 0, 0, 0),
                          false, 1000);
    model.observeCanFrame(0, nodeStatusCanId(43),
                          nodeStatusPayload(20, 0, 0, 0, 0),
                          false, 1000);
    model.SelectNode(42);
    QVERIFY(model.GetParameters());
    model.SelectNode(43);
    QCOMPARE(cancellations.count(), 1);
    QVERIFY(!model.IsReadingParameters());
    QVERIFY(model.Parameters().isEmpty());

    QVERIFY(model.GetParameters());
    model.parameterRequestFailed(43, QStringLiteral("timeout"));
    QVERIFY(!model.IsReadingParameters());
    QVERIFY(model.NodeStatus().contains(QStringLiteral("timeout")));

    QVERIFY(model.GetParameters());
    QVERIFY(model.Refresh());
    QCOMPARE(cancellations.count(), 2);
    QVERIFY(model.Parameters().isEmpty());
    QCOMPARE(model.SelectedNodeId(), -1);

    model.observeCanFrame(0, nodeStatusCanId(44),
                          nodeStatusPayload(20, 0, 0, 0, 0),
                          false, 2000);
    model.SelectNode(44);
    QVERIFY(model.GetParameters());
    model.Disconnect(QStringLiteral("link closed"));
    QCOMPARE(cancellations.count(), 3);
    QVERIFY(!model.IsReadingParameters());
    QVERIFY(!model.CanGetParameters());
}

void ConfigDroneCanViewTest::widgetMatchesMissionPlannerDiscoverySurface()
{
    ConfigDroneCanView view;
    view.setVehicleConnected(true);
    view.resize(view.sizeHint());
    view.show();
    QApplication::processEvents();

    QCOMPARE(view.objectName(), QStringLiteral("ConfigDroneCanView"));
    auto *title = view.findChild<QLabel *>(QStringLiteral("droneCanTitle"));
    auto *selector = view.findChild<QComboBox *>(
        QStringLiteral("DroneCanInterfaceSelector"));
    auto *connectButton = view.findChild<QPushButton *>(
        QStringLiteral("DroneCanConnectButton"));
    auto *refreshButton = view.findChild<QPushButton *>(
        QStringLiteral("DroneCanRefreshButton"));
    auto *nodes = view.findChild<QTableWidget *>(
        QStringLiteral("droneCanNodes"));
    auto *firmware = view.findChild<QPushButton *>(
        QStringLiteral("FirmwareUpdateBtn"));
    auto *getParameters = view.findChild<QPushButton *>(
        QStringLiteral("DroneCanGetParametersButton"));
    auto *write = view.findChild<QPushButton *>(
        QStringLiteral("DroneCanWriteButton"));
    auto *parameterSearch = view.findChild<QLineEdit *>(
        QStringLiteral("DroneCanParameterSearch"));
    auto *parameters = view.findChild<QTableWidget *>(
        QStringLiteral("droneCanNodeParams"));
    QVERIFY(title);
    QVERIFY(selector);
    QVERIFY(connectButton);
    QVERIFY(refreshButton);
    QVERIFY(nodes);
    QVERIFY(firmware);
    QVERIFY(getParameters);
    QVERIFY(write);
    QVERIFY(parameterSearch);
    QVERIFY(parameters);
    QCOMPARE(title->text(), QStringLiteral("DroneCAN / UAVCAN"));
    QCOMPARE(selector->count(), 2);
    QCOMPARE(connectButton->text(), QStringLiteral("Connect"));
    QCOMPARE(nodes->columnCount(), 9);
    QCOMPARE(nodes->horizontalHeaderItem(0)->text(), QStringLiteral("ID"));
    QCOMPARE(nodes->horizontalHeaderItem(1)->text(), QStringLiteral("Name"));
    QCOMPARE(nodes->horizontalHeaderItem(8)->text(), QStringLiteral("Menu"));
    QCOMPARE(parameters->columnCount(), 6);
    QCOMPARE(parameters->horizontalHeaderItem(1)->text(),
             QStringLiteral("Name"));
    QVERIFY(!firmware->isEnabled());
    QVERIFY(!getParameters->isEnabled());
    QVERIFY(!write->isEnabled());
    QVERIFY(!parameterSearch->isEnabled());

    QSignalSpy forwarding(
        &view, &ConfigDroneCanView::canForwardingRequested);
    connectButton->click();
    QCOMPARE(forwarding.count(), 1);
    view.forwardingAckReceived(0);
    view.canFrameReceived(0, nodeStatusCanId(23),
                          nodeStatusPayload(44, 0, 0, 0, 0),
                          false, 1000);
    QCOMPARE(nodes->rowCount(), 1);
    QCOMPARE(nodes->item(0, 0)->text(), QStringLiteral("23"));
    view.viewModel()->SelectNode(23);
    QVERIFY(getParameters->isEnabled());
    QSignalSpy parameterRequests(
        view.viewModel(),
        &ConfigDroneCanViewModel::parameterReadRequested);
    getParameters->click();
    QCOMPARE(parameterRequests.count(), 1);
    QVERIFY(!getParameters->isEnabled());

    DroneCanGetSetClient::Parameter parameter;
    parameter.busIndex = 0;
    parameter.nodeId = 23;
    parameter.requestedIndex = 0;
    parameter.name = QByteArray("FOO_GAIN");
    parameter.value = DroneCanGetSetClient::Value::fromReal(1.5F);
    parameter.defaultValue = DroneCanGetSetClient::Value::fromReal(1.0F);
    parameter.minimumValue.type =
        DroneCanGetSetClient::NumericValue::Real;
    parameter.minimumValue.realValue = 0.0F;
    parameter.maximumValue.type =
        DroneCanGetSetClient::NumericValue::Real;
    parameter.maximumValue.realValue = 2.0F;
    view.viewModel()->observeParameter(parameter);
    QCOMPARE(parameters->rowCount(), 1);
    QCOMPARE(parameters->item(0, 1)->text(), QStringLiteral("FOO_GAIN"));
    QCOMPARE(parameters->item(0, 2)->text(), QStringLiteral("1.5"));
    QCOMPARE(parameters->item(0, 3)->text(), QStringLiteral("0"));
    QCOMPARE(parameters->item(0, 4)->text(), QStringLiteral("2"));
    QCOMPARE(parameters->item(0, 5)->text(), QStringLiteral("1"));

    DroneCanGetSetClient::Parameter end;
    end.busIndex = 0;
    end.nodeId = 23;
    end.requestedIndex = 1;
    view.viewModel()->observeParameter(end);
    QVERIFY(getParameters->isEnabled());
    QVERIFY(parameterSearch->isEnabled());
    parameterSearch->setText(QStringLiteral("ZZ"));
    QVERIFY(parameters->isRowHidden(0));
    parameterSearch->setText(QStringLiteral("FO"));
    QVERIFY(!parameters->isRowHidden(0));
    QVERIFY(!write->isEnabled());
    QVERIFY(!firmware->isEnabled());
    QImage image(view.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    view.render(&image);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            QCOMPARE(qAlpha(image.pixel(x, y)), 255);
        }
    }
    QVERIFY(image.pixelColor(1, 1).lightness() < 100);
}

QTEST_MAIN(ConfigDroneCanViewTest)
#include "test_configdronecanview.moc"
