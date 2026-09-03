#include <QtTest>

#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/VehicleTargetManager.h"
#include "ui/DeviceOperationsWindow.h"

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>

#include <cstring>

namespace {

struct CapturedFrame
{
    int linkId = -1;
    QByteArray bytes;
};

VehicleEndpoint endpoint(int linkId = 9, int systemId = 42,
                         int componentId = 1)
{
    VehicleEndpoint result;
    result.linkId = linkId;
    result.systemId = systemId;
    result.componentId = componentId;
    result.linkName = QStringLiteral("Test Link");
    return result;
}

mavlink_message_t decodeFrame(const QByteArray &bytes)
{
    MAVLinkFrameParser parser;
    mavlink_message_t message{};
    unsigned int state = MAVLINK_FRAMING_INCOMPLETE;
    for (char byte : bytes) {
        state = parser.parseByte(static_cast<quint8>(byte), &message);
    }
    if (state != MAVLINK_FRAMING_OK) {
        std::memset(&message, 0, sizeof(message));
    }
    return message;
}

mavlink_message_t heartbeat(bool armed, quint8 systemId = 42,
                            quint8 componentId = 1)
{
    mavlink_heartbeat_t payload{};
    payload.base_mode = armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0;
    mavlink_message_t message{};
    mavlink_msg_heartbeat_encode(systemId, componentId, &message, &payload);
    return message;
}

mavlink_message_t readReply(quint32 requestId, quint8 result,
                            quint8 registerStart, const QByteArray &bytes,
                            quint8 systemId = 42,
                            quint8 componentId = 1)
{
    quint8 data[128]{};
    const int count = qMin(bytes.size(), 128);
    if (count > 0) {
        std::memcpy(data, bytes.constData(), static_cast<size_t>(count));
    }
    mavlink_message_t message{};
    mavlink_msg_device_op_read_reply_pack(
        systemId, componentId, &message, requestId, result,
        registerStart, static_cast<quint8>(count), data, 0);
    return message;
}

struct WindowFixture
{
    VehicleTargetManager targets;
    QList<CapturedFrame> frames;
    ExactLinkTransmitter transmitter;
    bool confirmationResult = false;
    int confirmationCalls = 0;
    QString confirmationTitle;
    QString confirmationMessage;

    WindowFixture()
        : transmitter([this](int linkId, const QByteArray &bytes) {
            frames.append({linkId, bytes});
            return true;
        })
    {
    }

    void selectTarget()
    {
        const VehicleEndpoint target = endpoint();
        targets.observeEndpoint(target);
        QVERIFY(targets.selectTarget(target.linkId, target.systemId,
                                     target.componentId));
    }

    DeviceOperationsWindow::Dependencies dependencies()
    {
        DeviceOperationsWindow::Dependencies result;
        result.targetManager = &targets;
        result.transmitter = &transmitter;
        result.localSystemId = 250;
        result.localComponentId = MAV_COMP_ID_MISSIONPLANNER;
        result.viewModel.resolveTarget = [this]() {
            return targets.acquireTarget();
        };
        result.viewModel.confirm = [this](const QString &title,
                                          const QString &message) {
            ++confirmationCalls;
            confirmationTitle = title;
            confirmationMessage = message;
            return confirmationResult;
        };
        return result;
    }
};

} // namespace

class DeviceOperationsWindowTest final : public QObject
{
    Q_OBJECT

private slots:
    void initialUiMatchesMissionPlanner10();
    void busSelectionUpdatesEnabledControls();
    void formatsStatusesAndAddressedHexRows();
    void readRegistersPublishesMatchedResult();
    void validationAndIcmSafetyGuardsFailClosed();
    void windowsAreModelessIndependentSessions();
};

void DeviceOperationsWindowTest::initialUiMatchesMissionPlanner10()
{
    WindowFixture fixture;
    DeviceOperationsWindow window(fixture.dependencies());

    QCOMPARE(window.objectName(), QStringLiteral("DeviceOperationsWindow"));
    QCOMPARE(window.windowTitle(), QStringLiteral("MAVLink Device Operations"));
    QCOMPARE(window.size(), QSize(760, 520));
    QCOMPARE(window.minimumSize(), QSize(700, 420));
    QCOMPARE(window.windowModality(), Qt::NonModal);
    QVERIFY(window.isWindow());
    QVERIFY(qobject_cast<QDialog *>(&window) == nullptr);
    QCOMPARE(window.findChild<QLabel *>(
                 QStringLiteral("deviceOperationsHeader"))->text(),
             QStringLiteral("MAVLink Device Operations"));
    QCOMPARE(window.findChild<QLabel *>(
                 QStringLiteral("deviceOperationsDescription"))->text(),
             QStringLiteral(
                 "Low-level ArduPilot DEVICE_OP access for SPI and I²C "
                 "peripherals. Values use decimal input; results are shown "
                 "in hexadecimal."));

    QCOMPARE(window.findChild<QSpinBox *>(
                 QStringLiteral("deviceOperationsSystemId"))->value(), 1);
    QCOMPARE(window.findChild<QSpinBox *>(
                 QStringLiteral("deviceOperationsComponentId"))->value(), 1);
    QCOMPARE(window.findChild<QComboBox *>(
                 QStringLiteral("deviceOperationsBusType"))->currentText(),
             QStringLiteral("SPI"));
    QCOMPARE(window.findChild<QLineEdit *>(
                 QStringLiteral("deviceOperationsBusName"))->text(),
             QStringLiteral("icm20948_ext"));
    QCOMPARE(window.findChild<QSpinBox *>(
                 QStringLiteral("deviceOperationsBusNumber"))->value(), 0);
    QCOMPARE(window.findChild<QSpinBox *>(
                 QStringLiteral("deviceOperationsAddress"))->value(), 0);
    QCOMPARE(window.findChild<QSpinBox *>(
                 QStringLiteral("deviceOperationsRegisterStart"))->value(),
             255);
    QCOMPARE(window.findChild<QSpinBox *>(
                 QStringLiteral("deviceOperationsCount"))->value(), 1);

    auto *output = window.findChild<QPlainTextEdit *>(
        QStringLiteral("deviceOperationsOutput"));
    QVERIFY(output->isReadOnly());
    QCOMPARE(output->lineWrapMode(), QPlainTextEdit::WidgetWidth);
    QCOMPARE(output->toPlainText(), QStringLiteral(
        "DEVICE_OP directly accesses a flight-controller peripheral bus. "
        "Use only with known hardware."));
    QVERIFY(!window.findChild<QProgressBar *>(
        QStringLiteral("deviceOperationsProgress"))->isVisible());
    QVERIFY(!window.findChild<QPushButton *>(
        QStringLiteral("ReadRegistersButton"))->isEnabled());
}

void DeviceOperationsWindowTest::busSelectionUpdatesEnabledControls()
{
    WindowFixture fixture;
    fixture.selectTarget();
    DeviceOperationsWindow window(fixture.dependencies());
    auto *busType = window.findChild<QComboBox *>(
        QStringLiteral("deviceOperationsBusType"));
    auto *busName = window.findChild<QLineEdit *>(
        QStringLiteral("deviceOperationsBusName"));
    auto *bus = window.findChild<QSpinBox *>(
        QStringLiteral("deviceOperationsBusNumber"));
    auto *address = window.findChild<QSpinBox *>(
        QStringLiteral("deviceOperationsAddress"));
    auto *test = window.findChild<QPushButton *>(
        QStringLiteral("TestIcm20948Button"));

    QVERIFY(busName->isEnabled());
    QVERIFY(!bus->isEnabled());
    QVERIFY(!address->isEnabled());
    QVERIFY(test->isEnabled());

    busType->setCurrentText(QStringLiteral("I2C"));
    QVERIFY(!busName->isEnabled());
    QVERIFY(bus->isEnabled());
    QVERIFY(address->isEnabled());
    QVERIFY(!test->isEnabled());

    busType->setCurrentText(QStringLiteral("SPI"));
    QVERIFY(busName->isEnabled());
    QVERIFY(!bus->isEnabled());
    QVERIFY(!address->isEnabled());
    QVERIFY(test->isEnabled());
}

void DeviceOperationsWindowTest::formatsStatusesAndAddressedHexRows()
{
    QCOMPARE(DeviceOperationsViewModel::FormatStatus(0),
             QStringLiteral("result 0 (OK)"));
    QCOMPARE(DeviceOperationsViewModel::FormatStatus(1),
             QStringLiteral("result 1 (bad bus)"));
    QCOMPARE(DeviceOperationsViewModel::FormatStatus(2),
             QStringLiteral("result 2 (bad device)"));
    QCOMPARE(DeviceOperationsViewModel::FormatStatus(3),
             QStringLiteral("result 3 (semaphore unavailable)"));
    QCOMPARE(DeviceOperationsViewModel::FormatStatus(4),
             QStringLiteral("result 4 (bad response)"));
    QCOMPARE(DeviceOperationsViewModel::FormatStatus(99),
             QStringLiteral("result 99 (unknown)"));

    QByteArray data;
    for (int value = 0; value < 18; ++value) {
        data.append(static_cast<char>(value));
    }
    const QString formatted = DeviceOperationsViewModel::FormatResult(
        0, 0xf8, data);
    QVERIFY(formatted.contains(QStringLiteral("18 byte(s)")));
    QVERIFY(formatted.contains(
        QStringLiteral("F8: 00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F")));
    QVERIFY(formatted.contains(QStringLiteral("08: 10 11")));
    QVERIFY(DeviceOperationsViewModel::FormatResult(
                0, 0, QByteArray(), true)
                .contains(QStringLiteral("timeout"),
                          Qt::CaseInsensitive));
    QVERIFY(DeviceOperationsViewModel::FormatResult(
                4, 0, QByteArray())
                .contains(QStringLiteral("result 4")));
}

void DeviceOperationsWindowTest::readRegistersPublishesMatchedResult()
{
    WindowFixture fixture;
    fixture.selectTarget();
    DeviceOperationsWindow window(fixture.dependencies());
    auto *read = window.findChild<QPushButton *>(
        QStringLiteral("ReadRegistersButton"));
    auto *progress = window.findChild<QProgressBar *>(
        QStringLiteral("deviceOperationsProgress"));
    auto *output = window.findChild<QPlainTextEdit *>(
        QStringLiteral("deviceOperationsOutput"));

    QVERIFY(read->isEnabled());
    read->click();
    QCOMPARE(fixture.frames.size(), 1);
    QVERIFY(window.service()->isBusy());
    QVERIFY(!read->isEnabled());
    // Hidden parents make isVisible() false, so inspect the explicit property.
    QVERIFY(!progress->isHidden());
    QCOMPARE(output->toPlainText(),
             QStringLiteral("Waiting for DEVICE_OP_READ_REPLY…"));

    const mavlink_message_t sent = decodeFrame(fixture.frames.first().bytes);
    QCOMPARE(sent.msgid, quint32(MAVLINK_MSG_ID_DEVICE_OP_READ));
    mavlink_device_op_read_t request{};
    mavlink_msg_device_op_read_decode(&sent, &request);
    QCOMPARE(fixture.frames.first().linkId, 9);
    QCOMPARE(request.target_system, quint8(42));
    QCOMPARE(request.target_component, quint8(1));
    QCOMPARE(request.bustype, quint8(DEVICE_OP_BUSTYPE_SPI));
    QCOMPARE(request.regstart, quint8(255));
    QCOMPARE(request.count, quint8(1));

    window.service()->observeMessage(
        9, readReply(request.request_id, 0, 255,
                     QByteArray::fromHex("ea")));
    QVERIFY(!window.service()->isBusy());
    QCOMPARE(output->toPlainText(), QStringLiteral(
        "DEVICE_OP succeeded: 1 byte(s).\nFF: EA"));
    QVERIFY(progress->isHidden());

    window.findChild<QLineEdit *>(
        QStringLiteral("deviceOperationsBusName"))->setText(
            QString(41, QLatin1Char('a')));
    read->click();
    QCOMPARE(fixture.frames.size(), 1);
    QVERIFY(output->toPlainText().contains(QStringLiteral("40-byte")));
}

void DeviceOperationsWindowTest::validationAndIcmSafetyGuardsFailClosed()
{
    WindowFixture fixture;
    fixture.selectTarget();
    DeviceOperationsWindow window(fixture.dependencies());
    auto *test = window.findChild<QPushButton *>(
        QStringLiteral("TestIcm20948Button"));
    auto *output = window.findChild<QPlainTextEdit *>(
        QStringLiteral("deviceOperationsOutput"));

    test->click();
    QCOMPARE(fixture.confirmationCalls, 0);
    QCOMPARE(fixture.frames.size(), 0);
    QVERIFY(output->toPlainText().contains(
        QStringLiteral("armed state is unknown")));

    window.service()->observeMessage(9, heartbeat(true));
    test->click();
    QCOMPARE(fixture.confirmationCalls, 0);
    QCOMPARE(fixture.frames.size(), 0);
    QVERIFY(output->toPlainText().contains(
        QStringLiteral("blocked while the selected vehicle is armed")));

    window.service()->observeMessage(9, heartbeat(false));
    window.findChild<QSpinBox *>(
        QStringLiteral("deviceOperationsSystemId"))->setValue(43);
    test->click();
    QCOMPARE(fixture.confirmationCalls, 0);
    QCOMPARE(fixture.frames.size(), 0);
    QVERIFY(output->toPlainText().contains(
        QStringLiteral("bound target 42:1")));
    window.findChild<QSpinBox *>(
        QStringLiteral("deviceOperationsSystemId"))->setValue(42);
    test->click();
    QCOMPARE(fixture.confirmationCalls, 1);
    QCOMPARE(fixture.frames.size(), 0); // injected Cancel/default
    QCOMPARE(fixture.confirmationTitle,
             QStringLiteral("ICM20948 DEVICE_OP Test"));
    QVERIFY(fixture.confirmationMessage.contains(
        QStringLiteral("write 72 00 to register FF")));

    fixture.confirmationResult = true;
    test->click();
    QCOMPARE(fixture.confirmationCalls, 2);
    QCOMPARE(fixture.frames.size(), 1);
    QVERIFY(window.service()->isBusy());
    const mavlink_message_t sent = decodeFrame(fixture.frames.first().bytes);
    QCOMPARE(sent.msgid, quint32(MAVLINK_MSG_ID_DEVICE_OP_WRITE));
    mavlink_device_op_write_t request{};
    mavlink_msg_device_op_write_decode(&sent, &request);
    QCOMPARE(request.regstart, quint8(0xff));
    QCOMPARE(request.count, quint8(2));
    QCOMPARE(request.data[0], quint8(0x72));
    QCOMPARE(request.data[1], quint8(0x00));
    window.viewModel()->cancel();
    QVERIFY(!window.service()->isBusy());

    window.findChild<QComboBox *>(
        QStringLiteral("deviceOperationsBusType"))->setCurrentText(
            QStringLiteral("I2C"));
    QVERIFY(!test->isEnabled());
}

void DeviceOperationsWindowTest::windowsAreModelessIndependentSessions()
{
    WindowFixture fixture;
    fixture.selectTarget();
    auto *first = new DeviceOperationsWindow(fixture.dependencies());
    auto *second = new DeviceOperationsWindow(fixture.dependencies());
    first->service()->setTimeoutMs(10000);
    second->service()->setTimeoutMs(10000);
    QPointer<DeviceOperationsWindow> firstGuard(first);
    QPointer<DeviceOperationsWindow> secondGuard(second);

    first->show();
    second->show();
    first->findChild<QPushButton *>(
        QStringLiteral("ReadRegistersButton"))->click();
    second->findChild<QPushButton *>(
        QStringLiteral("ReadRegistersButton"))->click();
    QCOMPARE(fixture.frames.size(), 2);
    mavlink_device_op_read_t firstRequest{};
    mavlink_device_op_read_t secondRequest{};
    const mavlink_message_t firstMessage =
        decodeFrame(fixture.frames.at(0).bytes);
    const mavlink_message_t secondMessage =
        decodeFrame(fixture.frames.at(1).bytes);
    mavlink_msg_device_op_read_decode(&firstMessage, &firstRequest);
    mavlink_msg_device_op_read_decode(&secondMessage, &secondRequest);
    QVERIFY(firstRequest.request_id != secondRequest.request_id);
    QVERIFY(first->service()->isBusy());
    QVERIFY(second->service()->isBusy());

    first->close();
    QTRY_VERIFY(firstGuard.isNull());
    QVERIFY(secondGuard);
    QVERIFY(second->service()->isBusy());

    second->close();
    QTRY_VERIFY(secondGuard.isNull());
}

QTEST_MAIN(DeviceOperationsWindowTest)

#include "test_deviceoperationswindow.moc"
