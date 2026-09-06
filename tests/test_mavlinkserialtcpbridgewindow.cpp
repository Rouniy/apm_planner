#include "ui/MavlinkSerialTcpBridgeWindow.h"

#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/MavlinkSerialTcpBridgeService.h"
#include "comm/SwarmTelemetryRegistry.h"
#include "comm/VehicleTargetManager.h"

#include <QtTest>

#include <QAbstractSocket>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QHostAddress>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>

#include <functional>

namespace
{
template<typename T>
T *control(QWidget *window, const char *name)
{
    T *result = window->findChild<T *>(QString::fromLatin1(name));
    Q_ASSERT(result);
    return result;
}

template<typename T>
T *visibleDialog(QWidget *window, const char *name)
{
    const auto dialogs = window->findChildren<T *>(
        QString::fromLatin1(name), Qt::FindChildrenRecursively);
    for (T *dialog : dialogs) {
        if (dialog->isVisible())
            return dialog;
    }
    return nullptr;
}

quint16 freePort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress(QHostAddress::LocalHost), quint16(0)))
        return 0;
    return probe.serverPort();
}

mavlink_message_t decodeFrame(const QByteArray &frame)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    for (char byte : frame)
        parser.parseByte(static_cast<quint8>(byte), &message);
    return message;
}

int releaseFrameCount(const QList<QByteArray> &frames)
{
    int releases = 0;
    for (const QByteArray &frame : frames) {
        const mavlink_message_t message = decodeFrame(frame);
        if (message.msgid != MAVLINK_MSG_ID_SERIAL_CONTROL)
            continue;
        mavlink_serial_control_t payload{};
        mavlink_msg_serial_control_decode(&message, &payload);
        if (payload.flags == 0)
            ++releases;
    }
    return releases;
}

class Fixture final
{
public:
    Fixture()
        : transmitter([this](int, const QByteArray &frame) {
              frames.append(frame);
              return writesSucceed;
          })
        , service(&targets, &registry, &transmitter, 250, 190,
                  [this](const SwarmVehicleInstanceLease &lease,
                         QString *error) {
                      if (routeHook)
                          routeHook();
                      if (!routeAllowed) {
                          if (error)
                              *error = QStringLiteral("Route unavailable.");
                          return false;
                      }
                      return registry.validateLease(lease, 3000);
                  })
    {
        endpoint.linkId = 74;
        endpoint.systemId = 42;
        endpoint.componentId = MAV_COMP_ID_AUTOPILOT1;
        endpoint.linkName = QStringLiteral("Serial bridge test link");
        epoch = registry.beginLinkSession(endpoint.linkId, endpoint.linkName);
        transmitter.setLinkSessionEpoch(endpoint.linkId, epoch);
        heartbeat(false);
    }

    void heartbeat(bool armed)
    {
        mavlink_message_t message{};
        mavlink_msg_heartbeat_pack(
            static_cast<quint8>(endpoint.systemId),
            static_cast<quint8>(endpoint.componentId), &message,
            MAV_TYPE_QUADROTOR, MAV_AUTOPILOT_ARDUPILOTMEGA,
            armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0,
            0, MAV_STATE_ACTIVE);
        QVERIFY(registry.observeMessage(endpoint.linkId, epoch, message));
        const QList<VehicleEndpoint> discovered = registry.endpoints();
        QVERIFY(!discovered.isEmpty());
        endpoint = discovered.constFirst();
        if (!targets.contains(endpoint.linkId, endpoint.systemId,
                              endpoint.componentId)) {
            QVERIFY(targets.observeEndpoint(endpoint, true));
        }
        targets.observeHeartbeat(endpoint, armed,
                                 MAV_AUTOPILOT_ARDUPILOTMEGA,
                                 MAV_TYPE_QUADROTOR);
    }

    VehicleTargetManager targets;
    SwarmTelemetryRegistry registry;
    QList<QByteArray> frames;
    bool writesSucceed = true;
    bool routeAllowed = true;
    std::function<void()> routeHook;
    ExactLinkTransmitter transmitter;
    MavlinkSerialTcpBridgeService service;
    VehicleEndpoint endpoint;
    quint64 epoch = 0;
};
} // namespace

class MavlinkSerialTcpBridgeWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void matchesMissionPlannerSurfaceAndDefaults();
    void confirmationIsDefaultCancelAndDoesNotBind();
    void confirmedStartUsesFrozenOptionsAndCloseStopsOwnedOperation();
    void targetChangeDuringConsentRejectsStart();
    void foreignOperationIsObservedButNeverStopped();
    void serviceDestructionDismissesConsent();
    void closingWindowCannotBeReusedBeforeDeferredDelete();
    void startCallbackRebindDoesNotRestoreOwnership();
};

void MavlinkSerialTcpBridgeWindowTest::matchesMissionPlannerSurfaceAndDefaults()
{
    QPointer<MavlinkSerialTcpBridgeWindow> window =
        new MavlinkSerialTcpBridgeWindow;
    QCOMPARE(window->objectName(), QStringLiteral("MavlinkSerialTcpBridgeWindow"));
    QCOMPARE(window->windowModality(), Qt::NonModal);
    QCOMPARE(window->size(), QSize(680, 570));
    QCOMPARE(window->minimumSize(), QSize(620, 520));
    QVERIFY(window->testAttribute(Qt::WA_DeleteOnClose));

    auto *device = control<QComboBox>(
        window, "MavlinkSerialTcpBridgeDeviceComboBox");
    auto *baud = control<QComboBox>(
        window, "MavlinkSerialTcpBridgeBaudComboBox");
    auto *port = control<QSpinBox>(
        window, "MavlinkSerialTcpBridgeListenPortSpinBox");
    auto *remote = control<QCheckBox>(
        window, "AllowRemoteSerialBridgeClientsCheckBox");
    auto *toggle = control<QPushButton>(
        window, "ToggleMavlinkSerialTcpBridgeButton");
    QCOMPARE(device->count(), 15);
    QCOMPARE(device->currentData().toUInt(),
             static_cast<uint>(SERIAL_CONTROL_DEV_GPS1));
    QCOMPARE(baud->count(), 10);
    QCOMPARE(baud->currentData().toUInt(), uint(0));
    QCOMPARE(port->minimum(), 1);
    QCOMPARE(port->maximum(), 65535);
    QCOMPARE(port->value(), 500);
    QVERIFY(!remote->isChecked());
    QVERIFY(!toggle->isEnabled());
    QVERIFY(!toggle->toolTip().isEmpty());
    QVERIFY(control<QLabel>(window,
        "MavlinkSerialTcpBridgeWarningLabel")->text().contains(
            QStringLiteral("no target-system")));
    QVERIFY(control<QLabel>(window,
        "MavlinkSerialTcpBridgeWarningLabel")->text().contains(
            QStringLiteral("SHELL")));
    QVERIFY(control<QLabel>(window,
        "MavlinkSerialTcpBridgeWarningLabel")->text().contains(
            QStringLiteral("100 ms")));

    delete window;
    QVERIFY(window.isNull());
}

void MavlinkSerialTcpBridgeWindowTest::confirmationIsDefaultCancelAndDoesNotBind()
{
    Fixture fixture;
    const quint16 portNumber = freePort();
    QVERIFY(portNumber != 0);
    MavlinkSerialTcpBridgeWindow window;
    window.setService(&fixture.service);
    window.show();

    auto *port = control<QSpinBox>(
        &window, "MavlinkSerialTcpBridgeListenPortSpinBox");
    auto *remote = control<QCheckBox>(
        &window, "AllowRemoteSerialBridgeClientsCheckBox");
    auto *toggle = control<QPushButton>(
        &window, "ToggleMavlinkSerialTcpBridgeButton");
    port->setValue(portNumber);
    remote->setChecked(true);
    QTRY_VERIFY(toggle->isEnabled());
    QTRY_VERIFY(control<QLabel>(&window,
        "MavlinkSerialTcpBridgeTargetLabel")->text().contains(
            fixture.endpoint.displayName()));
    toggle->click();

    auto *confirm = visibleDialog<QMessageBox>(
        &window, "MavlinkSerialTcpBridgeStartConfirmation");
    QVERIFY(confirm);
    QCOMPARE(confirm->textFormat(), Qt::PlainText);
    QCOMPARE(confirm->defaultButton(), confirm->button(QMessageBox::Cancel));
    QCOMPARE(confirm->escapeButton(), confirm->button(QMessageBox::Cancel));
    QVERIFY(confirm->text().contains(fixture.endpoint.displayName()));
    QVERIFY(confirm->text().contains(QString::number(portNumber)));
    QVERIFY(confirm->text().contains(QStringLiteral("every IPv4 interface")));
    QVERIFY(confirm->text().contains(QStringLiteral("not restored")));
    QVERIFY(confirm->text().contains(QStringLiteral("no acknowledgement")));
    QVERIFY(!fixture.service.busy());
    QVERIFY(fixture.frames.isEmpty());

    // Preparing and showing consent must not bind the TCP port.
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress(QHostAddress::LocalHost), portNumber));
    probe.close();
    confirm->button(QMessageBox::Cancel)->click();
    QVERIFY(!fixture.service.busy());
    QVERIFY(fixture.frames.isEmpty());
    QTRY_VERIFY(toggle->isEnabled());
}

void MavlinkSerialTcpBridgeWindowTest::confirmedStartUsesFrozenOptionsAndCloseStopsOwnedOperation()
{
    Fixture fixture;
    const quint16 selectedPort = freePort();
    QVERIFY(selectedPort != 0);
    QPointer<MavlinkSerialTcpBridgeWindow> window =
        new MavlinkSerialTcpBridgeWindow;
    window->setService(&fixture.service);
    window->show();

    auto *device = control<QComboBox>(
        window, "MavlinkSerialTcpBridgeDeviceComboBox");
    auto *baud = control<QComboBox>(
        window, "MavlinkSerialTcpBridgeBaudComboBox");
    auto *port = control<QSpinBox>(
        window, "MavlinkSerialTcpBridgeListenPortSpinBox");
    auto *remote = control<QCheckBox>(
        window, "AllowRemoteSerialBridgeClientsCheckBox");
    auto *toggle = control<QPushButton>(
        window, "ToggleMavlinkSerialTcpBridgeButton");
    device->setCurrentIndex(9); // SERIAL4
    baud->setCurrentIndex(6);  // 115200
    port->setValue(selectedPort);
    remote->setChecked(true);
    toggle->click();
    auto *confirm = visibleDialog<QMessageBox>(
        window, "MavlinkSerialTcpBridgeStartConfirmation");
    QVERIFY(confirm);
    QVERIFY(confirm->text().contains(QStringLiteral("SERIAL4")));

    // Programmatic changes cannot redirect the already frozen consent.
    device->setCurrentIndex(0);
    baud->setCurrentIndex(0);
    port->setValue(selectedPort == 65535 ? 65534 : selectedPort + 1);
    remote->setChecked(false);
    confirm->button(QMessageBox::Yes)->click();
    QTRY_VERIFY_WITH_TIMEOUT(fixture.service.busy(), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(window->ownsOperation(), 2000);
    const auto active = fixture.service.activePlan().options();
    QCOMPARE(active.device, static_cast<quint8>(SERIAL_CONTROL_SERIAL4));
    QCOMPARE(active.baudRate, quint32(115200));
    QCOMPARE(active.listenPort, selectedPort);
    QVERIFY(active.allowRemoteClients);
    QCOMPARE(control<QPushButton>(
        window, "ToggleMavlinkSerialTcpBridgeButton")->text(),
        QStringLiteral("Stop"));

    QTcpSocket client;
    client.connectToHost(QHostAddress(QHostAddress::LocalHost), selectedPort);
    QTRY_COMPARE_WITH_TIMEOUT(client.state(),
                              QAbstractSocket::ConnectedState, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(fixture.service.hasClient(), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.frames.isEmpty(), 2000);
    QCOMPARE(releaseFrameCount(fixture.frames), 0);

    window->close();
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.service.busy(), 2000);
    QCOMPARE(releaseFrameCount(fixture.frames), 1);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(window.isNull());
}

void MavlinkSerialTcpBridgeWindowTest::targetChangeDuringConsentRejectsStart()
{
    Fixture fixture;
    MavlinkSerialTcpBridgeWindow window;
    window.setService(&fixture.service);
    window.show();
    auto *toggle = control<QPushButton>(
        &window, "ToggleMavlinkSerialTcpBridgeButton");
    QTRY_VERIFY(toggle->isEnabled());
    toggle->click();
    auto *confirm = visibleDialog<QMessageBox>(
        &window, "MavlinkSerialTcpBridgeStartConfirmation");
    QVERIFY(confirm);

    fixture.targets.clearTarget();
    QVERIFY(fixture.targets.selectTarget(
        fixture.endpoint.linkId, fixture.endpoint.systemId,
        fixture.endpoint.componentId));
    fixture.heartbeat(false);
    confirm->button(QMessageBox::Yes)->click();
    QVERIFY(!fixture.service.busy());
    QVERIFY(!window.ownsOperation());
    QVERIFY(fixture.frames.isEmpty());
    QVERIFY(control<QLabel>(&window,
        "MavlinkSerialTcpBridgeStatusLabel")->text().contains(
            QStringLiteral("changed"), Qt::CaseInsensitive));
}

void MavlinkSerialTcpBridgeWindowTest::foreignOperationIsObservedButNeverStopped()
{
    Fixture fixture;
    MavlinkSerialTcpBridgeService::Options options;
    options.listenPort = freePort();
    QVERIFY(options.listenPort != 0);
    MavlinkSerialTcpBridgeService::Plan plan;
    QString error;
    QVERIFY2(fixture.service.prepare(options, &plan, &error), qPrintable(error));
    quint64 operationId = 0;
    QVERIFY2(fixture.service.start(plan, &operationId, &error), qPrintable(error));
    QVERIFY(operationId != 0);
    QVERIFY(fixture.service.busy());

    QPointer<MavlinkSerialTcpBridgeWindow> window =
        new MavlinkSerialTcpBridgeWindow;
    window->setService(&fixture.service);
    window->show();
    QVERIFY(!window->ownsOperation());
    auto *toggle = control<QPushButton>(
        window, "ToggleMavlinkSerialTcpBridgeButton");
    QVERIFY(!toggle->isEnabled());
    QVERIFY(toggle->toolTip().contains(QStringLiteral("Another")));
    window->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(window.isNull());
    QVERIFY(fixture.service.busy());
    QCOMPARE(fixture.service.operationId(), operationId);
    QVERIFY2(fixture.service.stop(operationId, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.service.busy(), 2000);
}

void MavlinkSerialTcpBridgeWindowTest::serviceDestructionDismissesConsent()
{
    Fixture fixture;
    MavlinkSerialTcpBridgeWindow window;
    auto *service = new MavlinkSerialTcpBridgeService(
        &fixture.targets, &fixture.registry, &fixture.transmitter,
        250, 190,
        [&fixture](const SwarmVehicleInstanceLease &lease, QString *) {
            return fixture.registry.validateLease(lease, 3000);
        });
    window.setService(service);
    window.show();
    auto *toggle = control<QPushButton>(
        &window, "ToggleMavlinkSerialTcpBridgeButton");
    QTRY_VERIFY(toggle->isEnabled());
    toggle->click();
    QVERIFY(visibleDialog<QMessageBox>(
        &window, "MavlinkSerialTcpBridgeStartConfirmation"));
    delete service;
    QVERIFY(!visibleDialog<QMessageBox>(
        &window, "MavlinkSerialTcpBridgeStartConfirmation"));
    QVERIFY(!toggle->isEnabled());
}

void MavlinkSerialTcpBridgeWindowTest::closingWindowCannotBeReusedBeforeDeferredDelete()
{
    Fixture fixture;
    QPointer<MavlinkSerialTcpBridgeWindow> window =
        new MavlinkSerialTcpBridgeWindow;
    window->setService(&fixture.service);
    window->show();
    auto *toggle = control<QPushButton>(
        window, "ToggleMavlinkSerialTcpBridgeButton");
    QTRY_VERIFY(toggle->isEnabled());

    window->close();
    QVERIFY(window);
    QVERIFY(window->isClosing());
    window->setService(nullptr);
    window->setService(&fixture.service);
    QVERIFY(window->isClosing());
    window->show(); // A stale singleton pointer must remain terminal.
    toggle->setEnabled(true);
    toggle->click();
    QVERIFY(!fixture.service.busy());
    QVERIFY(!visibleDialog<QMessageBox>(
        window, "MavlinkSerialTcpBridgeStartConfirmation"));

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(window.isNull());
}

void MavlinkSerialTcpBridgeWindowTest::startCallbackRebindDoesNotRestoreOwnership()
{
    Fixture fixture;
    MavlinkSerialTcpBridgeWindow window;
    window.setService(&fixture.service);
    window.show();
    auto *toggle = control<QPushButton>(
        &window, "ToggleMavlinkSerialTcpBridgeButton");
    QTRY_VERIFY(toggle->isEnabled());
    toggle->click();
    auto *confirm = visibleDialog<QMessageBox>(
        &window, "MavlinkSerialTcpBridgeStartConfirmation");
    QVERIFY(confirm);

    fixture.routeHook = [&]() {
        if (fixture.service.busy())
            window.setService(nullptr);
    };
    confirm->button(QMessageBox::Yes)->click();
    QVERIFY(!fixture.service.busy());
    QVERIFY(!window.ownsOperation());
    QCOMPARE(window.ownedOperationId(), quint64(0));
    QVERIFY(!toggle->isEnabled());
}

QTEST_MAIN(MavlinkSerialTcpBridgeWindowTest)

#include "test_mavlinkserialtcpbridgewindow.moc"
