#include <QtTest>

#include "ui/configuration/ConfigDeveloperToolsView.h"
#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/VehicleTargetManager.h"
#include "core/parameters/ParameterStore.h"

#include <QAction>
#include <QObject>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QProgressDialog>
#include <QRunnable>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtEndian>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QWidget>

#include <cstring>

namespace {
struct VehicleFixture
{
    VehicleTargetManager targets;
    SwarmTelemetryRegistry registry;
    QList<QByteArray> frames;
    ExactLinkTransmitter transmitter{[this](int, const QByteArray &frame) {
        frames.append(frame);
        return true;
    }};
    ParameterService parameters{&targets, &transmitter};
    VehicleCommandService commands{&targets, &transmitter};
    std::function<void()> routeHook;
    DeveloperVehicleToolService service{&targets, &registry, &parameters, &commands,
        [this](const SwarmVehicleInstanceLease &, QString *) {
            const auto hook = routeHook;
            if (hook)
                hook();
            return true;
        }};
    VehicleEndpoint endpoint;
    quint64 session = 0;

    VehicleFixture()
    {
        endpoint.linkId = 7;
        endpoint.systemId = 42;
        endpoint.componentId = 1;
        endpoint.linkName = QStringLiteral("Bench vehicle");
        session = registry.beginLinkSession(endpoint.linkId, endpoint.linkName);
        targets.observeEndpoint(endpoint, true);
        heartbeat(false);
        auto lease = [this](const SwarmVehicleInstanceLease &candidate) {
            return registry.validateLease(candidate);
        };
        auto route = [](const SwarmVehicleInstanceLease &, QString *) { return true; };
        parameters.configureExactTransactions(lease, route);
        parameters.configureSingleVehicleExactRoute(route);
        commands.configureExactTransactions(lease, route);
        commands.configureSingleVehicleExactRoute(route);
        commands.setLocalIdentity(250, 190);
        pressure(101325.0);
    }

    void heartbeat(bool armed)
    {
        mavlink_heartbeat_t payload{};
        payload.autopilot = MAV_AUTOPILOT_ARDUPILOTMEGA;
        payload.type = MAV_TYPE_QUADROTOR;
        payload.base_mode = armed ? MAV_MODE_FLAG_SAFETY_ARMED : 0;
        mavlink_message_t message{};
        mavlink_msg_heartbeat_encode(42, 1, &message, &payload);
        targets.observeHeartbeat(endpoint, armed, payload.autopilot, payload.type);
        registry.observeMessage(endpoint.linkId, session, message);
    }

    void pressure(double value)
    {
        parameters.store()->ingest(endpoint, 1, 0, QStringLiteral("GND_ABS_PRESS"),
                                   static_cast<float>(value), ParameterType::Real32);
    }

    void acknowledge(quint16 command)
    {
        mavlink_command_ack_t payload{};
        payload.command = command;
        payload.result = MAV_RESULT_ACCEPTED;
        payload.target_system = 250;
        payload.target_component = 190;
        mavlink_message_t message{};
        mavlink_msg_command_ack_encode(42, 1, &message, &payload);
        commands.observeMessage(endpoint.linkId, message);
    }

    mavlink_message_t lastMessage() const
    {
        MAVLinkFrameParser parser;
        mavlink_message_t message{};
        for (const auto byte : frames.last())
            parser.parseByte(static_cast<quint8>(byte), &message);
        return message;
    }
};

QPushButton *tool(ConfigDeveloperToolsView &view, const char *name)
{
    return view.findChild<QPushButton *>(QString::fromLatin1(name));
}

QMessageBox *confirmation(ConfigDeveloperToolsView &view)
{
    const auto boxes = view.findChildren<QMessageBox *>(QStringLiteral("DeveloperVehicleConfirmation"));
    for (auto *box : boxes)
        if (box->isVisible())
            return box;
    return nullptr;
}

QByteArray recordedPacket(const mavlink_message_t &message)
{
    QByteArray result(8, '\0');
    qToBigEndian<quint64>(1700000000000000ULL,
        reinterpret_cast<uchar *>(result.data()));
    uint8_t bytes[MAVLINK_MAX_PACKET_LEN]{};
    const int size = mavlink_msg_to_send_buffer(bytes, &message);
    result.append(reinterpret_cast<const char *>(bytes), size);
    return result;
}

QByteArray correctionLog()
{
    mavlink_gps_inject_data_t payload{};
    payload.len = 3;
    payload.data[0] = 0xd3;
    payload.data[1] = 0;
    payload.data[2] = 0x21;
    mavlink_message_t message{};
    mavlink_msg_gps_inject_data_encode(42, 1, &message, &payload);
    QByteArray result = recordedPacket(message);
    mavlink_gps_rtcm_data_t rtcm{};
    rtcm.flags = 7; // Deliberately fragmented: extraction preserves log order.
    rtcm.len = 2;
    rtcm.data[0] = 0;
    rtcm.data[1] = 0x43;
    mavlink_msg_gps_rtcm_data_encode(43, 2, &message, &rtcm);
    return result + recordedPacket(message);
}

bool writeFixture(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readFixture(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Keep the actual extraction queued until cancellation/destruction has happened,
// making lifecycle checks deterministic without timing or huge fixture files.
class PausedGlobalPool
{
public:
    PausedGlobalPool()
        : pool(QThreadPool::globalInstance()), previousMaximum(pool->maxThreadCount())
    {
        pool->setMaxThreadCount(1);
        pool->start(QRunnable::create([this]() { entered.release(); resume.acquire(); }));
        ready = entered.tryAcquire(1, 5000);
    }
    ~PausedGlobalPool()
    {
        resume.release();
        pool->waitForDone();
        pool->setMaxThreadCount(previousMaximum);
    }
    bool ready = false;
private:
    QThreadPool *pool;
    int previousMaximum;
    QSemaphore entered;
    QSemaphore resume;
};
}

class ConfigDeveloperToolsViewTest final : public QObject
{
    Q_OBJECT

private slots:
    void mirrorsMissionPlannerInventory();
    void sharedApplicationActionsOpenTools();
    void decodersAppendResultsAndErrors();
    void actionGridAdaptsToAvailableWidth();
    void wiredInventoryAndEligibility();
    void everyVehicleWriteRequiresDefaultCancel_data();
    void everyVehicleWriteRequiresDefaultCancel();
    void numericInputKeepsCapturedParameterSnapshot();
    void confirmationRejectsChangedTarget();
    void pressureWritesWaitForEcho_data();
    void pressureWritesWaitForEcho();
    void commandsSendExactPayloadOnlyAfterConsent_data();
    void commandsSendExactPayloadOnlyAfterConsent();
    void closeCancelsConsentButKeepsAdmittedOperation();
    void routeCallbackMayDeletePage();
    void gpsPickerCancellationIsOfflineAndNonDestructive();
    void gpsExtractionCompletesOffline_data();
    void gpsExtractionCompletesOffline();
    void gpsExtractionCancellationAndLifetime_data();
    void gpsExtractionCancellationAndLifetime();
    void gpsAndVehicleOperationsInterlock();
};

void ConfigDeveloperToolsViewTest::mirrorsMissionPlannerInventory()
{
    ConfigDeveloperToolsView view;
    QCOMPARE(view.objectName(), QStringLiteral("ConfigDeveloperToolsView"));
    QCOMPARE(view.Title(), QStringLiteral("Developer Tools"));
    QCOMPARE(view.ActionCount(), 32);
    QCOMPARE(view.ImplementedActionCount(), 3);
    QVERIFY(view.Log().contains(QStringLiteral("3 of 32")));

    const QList<QPushButton *> buttons = view.findChildren<QPushButton *>();
    QCOMPARE(buttons.size(), 32);
    int enabled = 0;
    for (QPushButton *button : buttons) {
        if (button->isEnabled()) {
            ++enabled;
        } else {
            QVERIFY(!button->toolTip().isEmpty());
        }
    }
    QCOMPARE(enabled, 3);
    QVERIFY(view.findChild<QPushButton *>(
        QStringLiteral("DecodeMavlinkPacketButton"))->isEnabled());
    QVERIFY(view.findChild<QPushButton *>(
        QStringLiteral("DecodeHardwareIdButton"))->isEnabled());
    QVERIFY(!view.findChild<QPushButton *>(
        QStringLiteral("RebootVehicleButton"))->isEnabled());
}

void ConfigDeveloperToolsViewTest::sharedApplicationActionsOpenTools()
{
    QObject actionSource;
    QAction deviceOperations(&actionSource);
    deviceOperations.setObjectName(
        QStringLiteral("actionMavlinkDeviceOperations"));
    QAction terrain(&actionSource);
    terrain.setObjectName(QStringLiteral("actionTerrain3D"));
    QAction osdVideo(&actionSource);
    osdVideo.setObjectName(QStringLiteral("actionOsdVideoOverlay"));
    bool deviceTriggered = false;
    bool terrainTriggered = false;
    bool osdVideoTriggered = false;
    connect(&deviceOperations, &QAction::triggered,
            this, [&deviceTriggered]() { deviceTriggered = true; });
    connect(&terrain, &QAction::triggered,
            this, [&terrainTriggered]() { terrainTriggered = true; });
    connect(&osdVideo, &QAction::triggered,
            this, [&osdVideoTriggered]() { osdVideoTriggered = true; });

    ConfigDeveloperToolsView view(&actionSource);
    QCOMPARE(view.ActionCount(), 32);
    QCOMPARE(view.ImplementedActionCount(), 6);
    QVERIFY(view.Log().contains(QStringLiteral("6 of 32")));
    auto *deviceButton = view.findChild<QPushButton *>(
        QStringLiteral("MavlinkDeviceOperationsButton"));
    auto *terrainButton = view.findChild<QPushButton *>(
        QStringLiteral("Terrain3dViewButton"));
    auto *osdVideoButton = view.findChild<QPushButton *>(
        QStringLiteral("OsdVideoTelemetryOverlayButton"));
    QVERIFY(deviceButton);
    QVERIFY(terrainButton);
    QVERIFY(osdVideoButton);
    QVERIFY(deviceButton->isEnabled());
    QVERIFY(terrainButton->isEnabled());
    QVERIFY(osdVideoButton->isEnabled());
    deviceButton->click();
    terrainButton->click();
    osdVideoButton->click();
    QVERIFY(deviceTriggered);
    QVERIFY(terrainTriggered);
    QVERIFY(osdVideoTriggered);
    QVERIFY(view.Log().contains(
        QStringLiteral("Opened MAVLink Device Operations.")));
    QVERIFY(view.Log().contains(QStringLiteral("Opened 3D Terrain View.")));
    QVERIFY(view.Log().contains(
        QStringLiteral("Opened OSD Video — Telemetry Overlay.")));

    deviceOperations.setEnabled(false);
    terrain.setEnabled(false);
    osdVideo.setEnabled(false);
    QVERIFY(!deviceButton->isEnabled());
    QVERIFY(!terrainButton->isEnabled());
    QVERIFY(!osdVideoButton->isEnabled());
}

void ConfigDeveloperToolsViewTest::decodersAppendResultsAndErrors()
{
    ConfigDeveloperToolsView view;
    view.ClearLog();

    view.DecodeHardwareIdInput(QStringLiteral("469530"),
                               QStringLiteral("COMPASS_DEV_ID"));
    QVERIFY(view.Log().contains(
        QStringLiteral("bus type SPI bus 3 address 42 devtype HMC5883")));

    view.DecodeHardwareIdInput(QStringLiteral("not-an-id"));
    QVERIFY(view.Log().contains(QStringLiteral("Hardware ID decode failed")));

    view.DecodeMavlinkInput(QStringLiteral("01 02 03"));
    QVERIFY(view.Log().contains(QStringLiteral("MAVLink decode failed")));
    QVERIFY(view.Log().contains(QStringLiteral("start byte")));
}

void ConfigDeveloperToolsViewTest::actionGridAdaptsToAvailableWidth()
{
    ConfigDeveloperToolsView view;
    view.resize(900, 720);
    view.show();
    QCoreApplication::processEvents();

    const int wideColumns = view.ColumnCount();
    QVERIFY(wideColumns >= 2);
    QVERIFY(wideColumns <= 4);

    view.resize(520, 720);
    QCoreApplication::processEvents();
    QVERIFY(view.ColumnCount() >= 1);
    QVERIFY(view.ColumnCount() < wideColumns);

    auto *host = view.findChild<QWidget *>(QStringLiteral("ActionItemsPanel"));
    auto *scroll = view.findChild<QScrollArea *>(QStringLiteral("ActionItemsScroll"));
    QVERIFY(host);
    QVERIFY(scroll);
    QVERIFY(scroll->height() <= scroll->maximumHeight());
    QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
    for (QPushButton *button : view.findChildren<QPushButton *>()) {
        QVERIFY2(button->geometry().right() <= host->contentsRect().right() + 1,
                 qPrintable(button->objectName()));
    }
}

void ConfigDeveloperToolsViewTest::wiredInventoryAndEligibility()
{
    VehicleFixture fixture;
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.service);
    QCOMPARE(view.ActionCount(), 32);
    QCOMPARE(view.ImplementedActionCount(), 9);
    QVERIFY(tool(view, "SetQnhButton")->isEnabled());
    QVERIFY(tool(view, "RebootVehicleButton")->isEnabled());
    fixture.heartbeat(true);
    QTRY_VERIFY(!tool(view, "RebootVehicleButton")->isEnabled());
    QVERIFY(!tool(view, "RebootVehicleButton")->toolTip().isEmpty());
    fixture.heartbeat(false);
    QTRY_VERIFY(tool(view, "RebootVehicleButton")->isEnabled());
    fixture.registry.endLinkSession(fixture.endpoint.linkId, fixture.session);
    QTRY_VERIFY(!tool(view, "RebootVehicleButton")->isEnabled());
    view.setVehicleToolService(nullptr);
    QCOMPARE(view.ImplementedActionCount(), 3);
    QVERIFY(!tool(view, "SetQnhButton")->isEnabled());
    QVERIFY(fixture.frames.isEmpty());
}

void ConfigDeveloperToolsViewTest::everyVehicleWriteRequiresDefaultCancel_data()
{
    QTest::addColumn<QString>("buttonName");
    for (const char *name : {"SetQnhButton", "AdjustBarometerAltitudeButton",
                            "ForceAccelCalibratedButton", "ForceCompassCalibratedButton",
                            "RebootVehicleButton", "RebootToDfuButton"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

void ConfigDeveloperToolsViewTest::everyVehicleWriteRequiresDefaultCancel()
{
    QFETCH(QString, buttonName);
    VehicleFixture fixture;
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.service);
    view.show();
    auto *button = view.findChild<QPushButton *>(buttonName);
    QVERIFY(button->isEnabled());
    button->click();
    if (auto *input = view.findChild<QInputDialog *>(QStringLiteral("DeveloperVehicleValueDialog"))) {
        QVERIFY(input->findChild<QDoubleSpinBox *>()->decimals() >= 3);
        input->setDoubleValue(buttonName == QStringLiteral("SetQnhButton") ? 101300.125 : 1.125);
        input->accept();
    }
    auto *dialog = confirmation(view);
    QVERIFY(dialog);
    QCOMPARE(dialog->textFormat(), Qt::PlainText);
    QCOMPARE(dialog->defaultButton(), dialog->button(QMessageBox::Cancel));
    QCOMPARE(dialog->escapeButton(), dialog->button(QMessageBox::Cancel));
    QVERIFY(dialog->text().contains(QStringLiteral("system 42, component 1")));
    QVERIFY(dialog->text().contains(QStringLiteral("Bench vehicle")));
    if (buttonName.startsWith(QStringLiteral("Force")))
        QVERIFY(dialog->text().contains(QStringLiteral("WITHOUT performing calibration")));
    if (buttonName == QStringLiteral("RebootToDfuButton"))
        QVERIFY(dialog->text().contains(QStringLiteral("does not confirm DFU entry")));
    QVERIFY(fixture.frames.isEmpty());
    dialog->button(QMessageBox::Cancel)->click();
    QVERIFY(fixture.frames.isEmpty());
    QVERIFY(!fixture.service.busy());
}

void ConfigDeveloperToolsViewTest::numericInputKeepsCapturedParameterSnapshot()
{
    VehicleFixture fixture;
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.service);
    view.show();
    tool(view, "SetQnhButton")->click();
    auto *input = view.findChild<QInputDialog *>(QStringLiteral("DeveloperVehicleValueDialog"));
    QVERIFY(input);
    fixture.pressure(100000.0);
    input->setDoubleValue(101000.0);
    input->accept();
    QVERIFY(!confirmation(view));
    QVERIFY(fixture.frames.isEmpty());
    QVERIFY(view.Log().contains(QStringLiteral("cancelled")));
}

void ConfigDeveloperToolsViewTest::confirmationRejectsChangedTarget()
{
    VehicleFixture fixture;
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.service);
    view.show();
    tool(view, "RebootVehicleButton")->click();
    auto *dialog = confirmation(view);
    QVERIFY(dialog);
    VehicleEndpoint other = fixture.endpoint;
    other.linkId = 8; // Identical MAVLink IDs on another physical transport.
    QVERIFY(fixture.targets.observeEndpoint(other));
    QVERIFY(fixture.targets.selectTarget(8, 42, 1));
    QVERIFY(fixture.targets.selectTarget(7, 42, 1));
    dialog->button(QMessageBox::Yes)->click();
    QVERIFY(fixture.frames.isEmpty());
    QVERIFY(view.Log().contains(QStringLiteral("cancelled")));
}

void ConfigDeveloperToolsViewTest::pressureWritesWaitForEcho_data()
{
    QTest::addColumn<bool>("adjustAltitude");
    QTest::newRow("QNH in Pa") << false;
    QTest::newRow("metres at 11.1 Pa per metre") << true;
}

void ConfigDeveloperToolsViewTest::pressureWritesWaitForEcho()
{
    QFETCH(bool, adjustAltitude);
    VehicleFixture fixture;
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.service);
    view.show();
    tool(view, adjustAltitude ? "AdjustBarometerAltitudeButton" : "SetQnhButton")->click();
    auto *input = view.findChild<QInputDialog *>(QStringLiteral("DeveloperVehicleValueDialog"));
    QVERIFY(input);
    const double value = adjustAltitude ? 10.0 : 101000.125;
    input->setDoubleValue(value);
    input->accept();
    auto *dialog = confirmation(view);
    QVERIFY(dialog);
    QVERIFY(dialog->text().contains(QStringLiteral("ground-pressure reference")));
    dialog->button(QMessageBox::Yes)->click();
    QCOMPARE(fixture.frames.size(), 1);
    QVERIFY(fixture.service.busy());
    QVERIFY(!tool(view, "RebootVehicleButton")->isEnabled());
    const auto message = fixture.lastMessage();
    QCOMPARE(message.msgid, static_cast<quint32>(MAVLINK_MSG_ID_PARAM_SET));
    mavlink_param_set_t sent{};
    mavlink_msg_param_set_decode(&message, &sent);
    QCOMPARE(sent.target_system, quint8(42));
    QCOMPARE(sent.target_component, quint8(1));
    QCOMPARE(sent.param_value, static_cast<float>(adjustAltitude ? 101436.0 : value));
    mavlink_param_value_t echo{};
    echo.param_value = sent.param_value;
    echo.param_type = sent.param_type;
    echo.param_count = 1;
    std::memcpy(echo.param_id, sent.param_id, sizeof echo.param_id);
    mavlink_message_t response{};
    mavlink_msg_param_value_encode(42, 1, &response, &echo);
    fixture.parameters.observeMessage(7, response);
    QVERIFY(!fixture.service.busy());
    QCOMPARE(fixture.service.lastReport().outcome,
             DeveloperVehicleToolService::Outcome::Succeeded);
    QVERIFY(view.Log().contains(fixture.service.lastReport().description));
}

void ConfigDeveloperToolsViewTest::commandsSendExactPayloadOnlyAfterConsent_data()
{
    QTest::addColumn<QString>("buttonName");
    QTest::addColumn<int>("command");
    QTest::addColumn<QList<float>>("parameters");
    QTest::newRow("force accelerometer") << QStringLiteral("ForceAccelCalibratedButton")
        << int(MAV_CMD_PREFLIGHT_CALIBRATION) << QList<float>({0, 0, 0, 0, 76, 0, 0});
    QTest::newRow("force compass") << QStringLiteral("ForceCompassCalibratedButton")
        << int(MAV_CMD_PREFLIGHT_CALIBRATION) << QList<float>({0, 76, 0, 0, 0, 0, 0});
    QTest::newRow("reboot") << QStringLiteral("RebootVehicleButton")
        << int(MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN) << QList<float>({1, 0, 0, 0, 0, 0, 0});
    QTest::newRow("ROM DFU") << QStringLiteral("RebootToDfuButton")
        << int(MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN) << QList<float>({42, 24, 71, 99, 0, 0, 0});
}

void ConfigDeveloperToolsViewTest::commandsSendExactPayloadOnlyAfterConsent()
{
    QFETCH(QString, buttonName);
    QFETCH(int, command);
    QFETCH(QList<float>, parameters);
    VehicleFixture fixture;
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.service);
    view.show();
    view.findChild<QPushButton *>(buttonName)->click();
    auto *dialog = confirmation(view);
    QVERIFY(dialog);
    QVERIFY(fixture.frames.isEmpty());
    dialog->button(QMessageBox::Yes)->click();
    QCOMPARE(fixture.frames.size(), 1);
    const auto message = fixture.lastMessage();
    QCOMPARE(message.msgid, static_cast<quint32>(MAVLINK_MSG_ID_COMMAND_LONG));
    mavlink_command_long_t sent{};
    mavlink_msg_command_long_decode(&message, &sent);
    QCOMPARE(sent.command, static_cast<quint16>(command));
    QCOMPARE(sent.target_system, quint8(42));
    QCOMPARE(sent.target_component, quint8(1));
    QCOMPARE(QList<float>({sent.param1, sent.param2, sent.param3, sent.param4,
                          sent.param5, sent.param6, sent.param7}), parameters);
    QVERIFY(!fixture.service.lastReport().isValid() ||
            fixture.service.lastReport().outcome != DeveloperVehicleToolService::Outcome::Succeeded);
    fixture.acknowledge(static_cast<quint16>(command));
    QVERIFY(!fixture.service.busy());
    QVERIFY(view.Log().contains(fixture.service.lastReport().description));
}

void ConfigDeveloperToolsViewTest::closeCancelsConsentButKeepsAdmittedOperation()
{
    VehicleFixture fixture;
    auto *view = new ConfigDeveloperToolsView;
    view->setVehicleToolService(&fixture.service);
    view->show();
    tool(*view, "RebootVehicleButton")->click();
    QPointer<QMessageBox> pending = confirmation(*view);
    QVERIFY(pending);
    view->close();
    if (pending)
        pending->done(QMessageBox::Yes); // A delayed acceptance must not transmit.
    QVERIFY(fixture.frames.isEmpty());
    view->show();
    QTRY_VERIFY(tool(*view, "RebootVehicleButton")->isEnabled());
    tool(*view, "RebootVehicleButton")->click();
    auto *dialog = confirmation(*view);
    QVERIFY(dialog);
    dialog->button(QMessageBox::Yes)->click();
    QCOMPARE(fixture.frames.size(), 1);
    QVERIFY(fixture.service.busy());
    delete view;
    QVERIFY(fixture.service.busy());
    fixture.acknowledge(MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN);
    QVERIFY(!fixture.service.busy());
    ConfigDeveloperToolsView reopened;
    reopened.setVehicleToolService(&fixture.service);
    QVERIFY(reopened.Log().contains(fixture.service.lastReport().description));
}

void ConfigDeveloperToolsViewTest::routeCallbackMayDeletePage()
{
    VehicleFixture fixture;
    QPointer<ConfigDeveloperToolsView> view = new ConfigDeveloperToolsView;
    view->setVehicleToolService(&fixture.service);
    view->show();
    tool(*view, "RebootVehicleButton")->click();
    auto *dialog = confirmation(*view);
    QVERIFY(dialog);
    fixture.routeHook = [&view]() { delete view.data(); };
    dialog->button(QMessageBox::Yes)->click();
    QVERIFY(view.isNull());
    QVERIFY(fixture.frames.isEmpty());
}

void ConfigDeveloperToolsViewTest::gpsPickerCancellationIsOfflineAndNonDestructive()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.tlog"));
    QVERIFY(writeFixture(input, correctionLog()));
    ConfigDeveloperToolsView view;
    view.show();
    auto *button = tool(view, "ExtractGpsCorrectionsButton");
    QVERIFY(button->isEnabled());
    button->click();
    auto *picker = view.findChild<QFileDialog *>(QStringLiteral("DeveloperGpsInputDialog"));
    QVERIFY(picker);
    QVERIFY(picker->testOption(QFileDialog::DontUseNativeDialog));
    QCOMPARE(picker->fileMode(), QFileDialog::ExistingFile);
    QVERIFY(!button->isEnabled());
    picker->reject();
    QVERIFY(button->isEnabled());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    button->click();
    picker = view.findChild<QFileDialog *>(QStringLiteral("DeveloperGpsInputDialog"));
    QVERIFY(picker);
    picker->selectFile(input);
    QVERIFY(QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection));
    auto *output = view.findChild<QFileDialog *>(QStringLiteral("DeveloperGpsOutputDialog"));
    QVERIFY(output);
    QCOMPARE(output->acceptMode(), QFileDialog::AcceptSave);
    QVERIFY(!output->testOption(QFileDialog::DontConfirmOverwrite));
    QVERIFY(output->selectedFiles().first().endsWith(QStringLiteral("flight-corrections.dat")));
    output->reject();
    QVERIFY(button->isEnabled());
    QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("flight-corrections.dat"))));
    QVERIFY(!view.findChild<QProgressDialog *>(QStringLiteral("DeveloperGpsProgressDialog")));
}

void ConfigDeveloperToolsViewTest::gpsExtractionCompletesOffline_data()
{
    QTest::addColumn<QString>("mode");
    QTest::newRow("two senders and binary zeros") << QStringLiteral("valid");
    QTest::newRow("no correction messages") << QStringLiteral("empty");
    QTest::newRow("truncated tail warning") << QStringLiteral("truncated");
    QTest::newRow("missing input") << QStringLiteral("missing");
}

void ConfigDeveloperToolsViewTest::gpsExtractionCompletesOffline()
{
    QFETCH(QString, mode);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.tlog"));
    const QString output = directory.filePath(QStringLiteral("corrections.dat"));
    QByteArray log = correctionLog();
    if (mode == QStringLiteral("empty")) {
        mavlink_message_t heartbeat{};
        mavlink_msg_heartbeat_pack(42, 1, &heartbeat, MAV_TYPE_QUADROTOR,
            MAV_AUTOPILOT_ARDUPILOTMEGA, 0, 0, MAV_STATE_STANDBY);
        log = recordedPacket(heartbeat);
    } else if (mode == QStringLiteral("truncated")) {
        log.append("bad", 3);
    }
    if (mode != QStringLiteral("missing"))
        QVERIFY(writeFixture(input, log));
    ConfigDeveloperToolsView view;
    view.show();
    view.ExtractGpsCorrections(input, output);
    auto *button = tool(view, "ExtractGpsCorrectionsButton");
    QVERIFY(!button->isEnabled());
    QVERIFY(view.findChild<QProgressDialog *>(QStringLiteral("DeveloperGpsProgressDialog")));
    QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 5000);
    if (mode == QStringLiteral("missing")) {
        QVERIFY(!QFile::exists(output));
        QVERIFY(view.Log().contains(QStringLiteral("extraction failed")));
        return;
    }
    QVERIFY(QFile::exists(output));
    QCOMPARE(readFixture(output), mode == QStringLiteral("empty")
             ? QByteArray() : QByteArray::fromHex("d300210043"));
    QVERIFY(view.Log().contains(QStringLiteral("extraction completed")));
    QVERIFY(view.Log().contains(QStringLiteral("no RTCM reassembly or validation")));
    QVERIFY(view.Log().contains(QStringLiteral("Older Qt logs may omit")));
    QVERIFY(view.Log().contains(QStringLiteral("while recording is enabled")));
    if (mode == QStringLiteral("empty"))
        QVERIFY(view.Log().contains(QStringLiteral("no GPS correction messages found")));
    if (mode == QStringLiteral("truncated")) {
        QVERIFY(view.Log().contains(QStringLiteral("extraction warning")));
        QVERIFY(view.Log().contains(QStringLiteral("truncated tail: yes")));
    }
}

void ConfigDeveloperToolsViewTest::gpsExtractionCancellationAndLifetime_data()
{
    QTest::addColumn<QString>("operation");
    QTest::newRow("cancel") << QStringLiteral("cancel");
    QTest::newRow("close") << QStringLiteral("close");
    QTest::newRow("destroy") << QStringLiteral("destroy");
}

void ConfigDeveloperToolsViewTest::gpsExtractionCancellationAndLifetime()
{
    QFETCH(QString, operation);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.tlog"));
    const QString output = directory.filePath(QStringLiteral("corrections.dat"));
    const QString secondOutput = directory.filePath(QStringLiteral("must-not-exist.dat"));
    QVERIFY(writeFixture(input, correctionLog()));
    QVERIFY(writeFixture(output, QByteArray("preserve old output")));
    QPointer<ConfigDeveloperToolsView> view = new ConfigDeveloperToolsView;
    view->show();
    QString logAtClose;
    {
        PausedGlobalPool paused;
        QVERIFY(paused.ready);
        view->ExtractGpsCorrections(input, output);
        view->ExtractGpsCorrections(input, secondOutput);
        QVERIFY(view->Log().contains(QStringLiteral("already active")));
        auto *progress = view->findChild<QProgressDialog *>(QStringLiteral("DeveloperGpsProgressDialog"));
        QVERIFY(progress);
        if (operation == QStringLiteral("destroy"))
            delete view.data();
        else if (operation == QStringLiteral("close")) {
            logAtClose = view->Log();
            view->close();
        } else {
            auto *cancel = progress->findChild<QPushButton *>();
            QVERIFY(cancel);
            cancel->click();
        }
    } // Allow the real, already-cancelled worker to drain.
    QCoreApplication::processEvents();
    QCOMPARE(readFixture(output), QByteArray("preserve old output"));
    QVERIFY(!QFile::exists(secondOutput));
    if (operation == QStringLiteral("destroy")) {
        QVERIFY(view.isNull());
    } else if (operation == QStringLiteral("close")) {
        QCOMPARE(view->Log(), logAtClose); // No result or progress touches closed UI.
        view->show();
        QTRY_VERIFY(tool(*view, "ExtractGpsCorrectionsButton")->isEnabled());
        delete view.data();
    } else {
        QTRY_VERIFY(view->Log().contains(QStringLiteral("extraction cancelled")));
        QVERIFY(tool(*view, "ExtractGpsCorrectionsButton")->isEnabled());
        delete view.data();
    }
}

void ConfigDeveloperToolsViewTest::gpsAndVehicleOperationsInterlock()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.tlog"));
    const QString output = directory.filePath(QStringLiteral("corrections.dat"));
    QVERIFY(writeFixture(input, correctionLog()));
    VehicleFixture fixture;
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.service);
    view.show();
    tool(view, "RebootVehicleButton")->click();
    auto *consent = confirmation(view);
    QVERIFY(consent);
    QVERIFY(!tool(view, "ExtractGpsCorrectionsButton")->isEnabled());
    view.ExtractGpsCorrections(input, output);
    QVERIFY(!view.findChild<QProgressDialog *>(QStringLiteral("DeveloperGpsProgressDialog")));
    consent->button(QMessageBox::Cancel)->click();
    {
        PausedGlobalPool paused;
        QVERIFY(paused.ready);
        view.ExtractGpsCorrections(input, output);
        QVERIFY(!tool(view, "RebootVehicleButton")->isEnabled());
        QVERIFY(!tool(view, "SetQnhButton")->isEnabled());
        tool(view, "RebootVehicleButton")->click();
        QVERIFY(fixture.frames.isEmpty());
        auto *progress = view.findChild<QProgressDialog *>(QStringLiteral("DeveloperGpsProgressDialog"));
        QVERIFY(progress);
        progress->findChild<QPushButton *>()->click();
    }
    QTRY_VERIFY(tool(view, "RebootVehicleButton")->isEnabled());
    QVERIFY(!QFile::exists(output));
}

QTEST_MAIN(ConfigDeveloperToolsViewTest)

#include "test_configdevelopertoolsview.moc"
