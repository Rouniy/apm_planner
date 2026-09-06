#include <QtTest>

#include "ui/configuration/ConfigDeveloperToolsView.h"
#include "comm/ExactLinkTransmitter.h"
#include "comm/MAVLinkFrameParser.h"
#include "comm/MavlinkComponentRegistry.h"
#include "comm/MavFtpServiceInterface.h"
#include "comm/RemoteDataFlashLogService.h"
#include "comm/VehicleTargetManager.h"
#include "core/parameters/ParameterStore.h"
#include "services/CameraProbeService.h"
#include "ui/Loghandling/DataFlashLogSplitter.h"

#include <QAction>
#include <QObject>
#include <QDoubleSpinBox>
#include <QDir>
#include <QHeaderView>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QRunnable>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTreeWidget>
#include <QtEndian>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QWidget>

#include <cstring>

namespace {
class DeveloperFtpStub final : public MavFtpServiceInterface
{
public:
    bool sharedBusy = false;
    bool isBusy() const override { return sharedBusy; }
    void setSharedBusy(bool busy)
    {
        sharedBusy = busy;
        emit stateChanged();
    }
    Operation operation() const override { return Operation::None; }
    quint64 activeTargetGeneration() const override { return 0; }
    QString lastError() const override { return {}; }
    StartResult startList(const QString &) override { return StartResult::TransportUnavailable; }
    StartResult startDownload(const QString &) override { return StartResult::TransportUnavailable; }
    StartResult startUpload(const QString &, const QByteArray &) override { return StartResult::TransportUnavailable; }
    StartResult startMakeDirectory(const QString &) override { return StartResult::TransportUnavailable; }
    StartResult startRemoveFile(const QString &) override { return StartResult::TransportUnavailable; }
    StartResult startRemoveDirectory(const QString &) override { return StartResult::TransportUnavailable; }
    void cancel() override { ++cancelCalls; }
    int cancelCalls = 0;
};

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
        return messageAt(frames.size() - 1);
    }

    mavlink_message_t messageAt(int index) const
    {
        MAVLinkFrameParser parser;
        mavlink_message_t message{};
        for (const auto byte : frames.at(index))
            parser.parseByte(static_cast<quint8>(byte), &message);
        return message;
    }

    void parameterReply(const QString &name, float value,
                        MAV_PARAM_TYPE type = MAV_PARAM_TYPE_REAL32,
                        int index = 0, int count = 4)
    {
        mavlink_param_value_t payload{};
        payload.param_value = value;
        payload.param_type = type;
        payload.param_count = static_cast<quint16>(count);
        payload.param_index = static_cast<quint16>(index);
        const QByteArray encodedName = name.toLatin1();
        std::memcpy(payload.param_id, encodedName.constData(),
                    qMin<int>(encodedName.size(), sizeof payload.param_id));
        mavlink_message_t response{};
        mavlink_msg_param_value_encode(42, 1, &response, &payload);
        parameters.observePhysicalMessage(endpoint.linkId, session, response);
    }
};

struct CameraProbePageFixture
{
    VehicleFixture vehicle;
    MavlinkComponentRegistry components;
    CameraProbeService service{
        &vehicle.targets, &components, &vehicle.commands,
        [](const MavlinkComponentInstanceLease &, QString *) {
            return true;
        }};
    bool configured = false;

    CameraProbePageFixture()
    {
        vehicle.transmitter.setLinkSessionEpoch(
            vehicle.endpoint.linkId, vehicle.session);
        configured = vehicle.commands.configureComponentExactTransactions(
            [this](const MavlinkComponentInstanceLease &lease) {
                return components.validateLease(lease);
            }, [](const MavlinkComponentInstanceLease &, QString *) {
                return true;
            });
        components.beginLinkSession(
            vehicle.endpoint.linkId, vehicle.session);
        mavlink_message_t heartbeat{};
        mavlink_msg_heartbeat_pack(
            static_cast<quint8>(vehicle.endpoint.systemId),
            MAV_COMP_ID_CAMERA, &heartbeat, MAV_TYPE_CAMERA,
            MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
        components.observeMessage(
            vehicle.endpoint.linkId, vehicle.session, heartbeat);
    }

    void acknowledge(MAV_CMD command,
                     MAV_RESULT result = MAV_RESULT_ACCEPTED)
    {
        mavlink_command_ack_t payload{};
        payload.command = static_cast<quint16>(command);
        payload.result = static_cast<quint8>(result);
        payload.progress = 255;
        payload.target_system = 250;
        payload.target_component = 190;
        mavlink_message_t message{};
        mavlink_msg_command_ack_encode(
            static_cast<quint8>(vehicle.endpoint.systemId),
            MAV_COMP_ID_CAMERA, &message, &payload);
        vehicle.commands.observeComponentMessage(
            vehicle.endpoint.linkId, vehicle.session, message);
    }
};

struct RemoteDataFlashFixture
{
    VehicleFixture vehicle;
    QTemporaryDir directory;
    RemoteDataFlashLogService service{
        &vehicle.targets, &vehicle.registry, &vehicle.parameters,
        &vehicle.transmitter, 250, 190,
        [](const SwarmVehicleInstanceLease &, QString *) { return true; }};
    bool ready = false;

    RemoteDataFlashFixture()
    {
        vehicle.transmitter.setLinkSessionEpoch(
            vehicle.endpoint.linkId, vehicle.session);
        auto *store = vehicle.parameters.store();
        store->beginLoad(vehicle.endpoint);
        const bool ingested = store->ingest(
            vehicle.endpoint, 1, 0,
            QStringLiteral("LOG_BACKEND_TYPE"),
            QVariant::fromValue<quint32>(2), ParameterType::UInt32);
        store->finishLoad(vehicle.endpoint);
        ready = ingested && store->snapshot(vehicle.endpoint).isComplete();
    }

    void block(quint32 sequence, char value = 'R')
    {
        mavlink_remote_log_data_block_t payload{};
        payload.seqno = sequence;
        payload.target_system = 250;
        payload.target_component = 190;
        std::memset(payload.data, value, sizeof payload.data);
        mavlink_message_t message{};
        mavlink_msg_remote_log_data_block_encode(
            42, MAV_COMP_ID_LOG, &message, &payload);
        service.observeMessage(vehicle.endpoint.linkId,
                               vehicle.session, message);
    }
};

QString mavlinkParameterName(const char *name, int size)
{
    int length = 0;
    while (length < size && name[length] != '\0')
        ++length;
    return QString::fromLatin1(name, length);
}

QPushButton *tool(ConfigDeveloperToolsView &view, const char *name)
{
    return view.findChild<QPushButton *>(QString::fromLatin1(name));
}

template<typename T>
T *visibleNamed(QWidget *parent, const char *name)
{
    const auto objects = parent->findChildren<T *>(QString::fromLatin1(name));
    for (T *object : objects) {
        if (object->isVisible())
            return object;
    }
    return nullptr;
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

QByteArray splittableAsciiLog(int dataRecords = 6)
{
    QByteArray log(
        "FMT,128,89,FMT,BBnNZ,Type,Length,Name,Format,Columns\n"
        "FMT,150,11,DUMY,Q,TimeUS\n");
    for (int index = 0; index < dataRecords; ++index) {
        log += "DUMY,";
        log += QByteArray::number(1000 + index);
        log += '\n';
    }
    return log;
}

QByteArray dashWareAsciiLog(int dataRecords = 2)
{
    QByteArray log(
        "FMT,128,89,FMT,BBnNZ,Type,Length,Name,Format,Columns\n"
        "FMT,150,19,GPS,Qff,TimeUS,Lat,Lng\n");
    for (int index = 0; index < dataRecords; ++index) {
        log += "GPS,";
        log += QByteArray::number((index + 1) * 1000000);
        log += ',';
        log += QByteArray::number(1.5 + 2.0 * index, 'f', 1);
        log += ',';
        log += QByteArray::number(2.5 + 2.0 * index, 'f', 1);
        log += '\n';
    }
    return log;
}

quint32 apjDescriptorCrc(const QByteArray &image, int begin, int end)
{
    quint32 value = 0;
    for (int at = begin; at < end; ++at) {
        value ^= quint8(image.at(at));
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1)
                ^ (0xedb88320U & (0U - (value & 1U)));
    }
    return value;
}

QByteArray embeddableApj(bool signedFirmware = false,
                         bool withDescriptor = false)
{
    QByteArray image(withDescriptor ? 160 : 100, 'x');
    image.replace(10, 16, QByteArray("PARMDEF\0", 8)
        + QByteArray::fromHex("5537f4a0385d485b"));
    qToLittleEndian<quint16>(32,
        reinterpret_cast<uchar *>(image.data() + 26));
    qToLittleEndian<quint16>(0,
        reinterpret_cast<uchar *>(image.data() + 28));
    if (withDescriptor) {
        constexpr int descriptor = 80;
        image.replace(descriptor, 8,
                      QByteArray::fromHex("40a2e4f164689106"));
        qToLittleEndian<quint32>(image.size(),
            reinterpret_cast<uchar *>(image.data() + descriptor + 16));
        qToLittleEndian<quint32>(apjDescriptorCrc(
            image, 0, descriptor + 8),
            reinterpret_cast<uchar *>(image.data() + descriptor + 8));
        qToLittleEndian<quint32>(apjDescriptorCrc(
            image, descriptor + 24, image.size()),
            reinterpret_cast<uchar *>(image.data() + descriptor + 12));
    }
    QJsonObject root;
    root.insert(QStringLiteral("magic"), QStringLiteral("APJFWv1"));
    root.insert(QStringLiteral("image_size"), image.size());
    root.insert(QStringLiteral("image"), QString::fromLatin1(
        qCompress(image, 9).mid(4).toBase64()));
    root.insert(QStringLiteral("signed_firmware"), signedFirmware);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
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
    void serialBridgeSharedActionTracksAvailability();
    void logIndexSharedActionTracksAvailability();
    void decodersAppendResultsAndErrors();
    void actionGridAdaptsToAvailableWidth();
    void wiredInventoryAndEligibility();
    void cameraProbeBindingAndGateRestoration();
    void everyVehicleWriteRequiresDefaultCancel_data();
    void everyVehicleWriteRequiresDefaultCancel();
    void numericInputKeepsCapturedParameterSnapshot();
    void confirmationRejectsChangedTarget();
    void pressureWritesWaitForEcho_data();
    void pressureWritesWaitForEcho();
    void commandsSendExactPayloadOnlyAfterConsent_data();
    void commandsSendExactPayloadOnlyAfterConsent();
    void upgradeBootloaderRequiresTwoConfirmations();
    void upgradeBootloaderRejectsStaleSecondConsent();
    void closeCancelsConsentButKeepsAdmittedOperation();
    void routeCallbackMayDeletePage();
    void gpsPickerCancellationIsOfflineAndNonDestructive();
    void gpsExtractionCompletesOffline_data();
    void gpsExtractionCompletesOffline();
    void gpsExtractionCancellationAndLifetime_data();
    void gpsExtractionCancellationAndLifetime();
    void gpsAndVehicleOperationsInterlock();
    void splitDialogsAreDefaultCancelAndNonDestructive();
    void splitCompletesOfflineAndReportsCounts();
    void splitRefusesExistingOutputs();
    void splitCancellationAndLifetime_data();
    void splitCancellationAndLifetime();
    void splitAndOtherOperationsInterlock();
    void dashWareDialogsAreCancellableAndUseMpDefaults();
    void dashWareExportCompletesOfflineAndReportsCounts();
    void dashWareFailurePreservesExistingOutput();
    void dashWareCancellationAndLifetime_data();
    void dashWareCancellationAndLifetime();
    void dashWareAndOtherOperationsInterlock();
    void mavFtpInjectionAndSharedOperationGate();
    void mavFtpServiceRemovalDisablesAction();
    void apjDialogsAreDefaultCancelAndPreserveExistingOutput();
    void apjEmbeddingCompletesAndSignedImagesFailClosed();
    void apjEmbeddingCancellationLifetimeAndInterlocks();
    void logOrganizerDialogsAreDefaultCancelAndReadOnly();
    void logOrganizerExecutesExactPlanAndNeverClobbers();
    void logOrganizerCancellationLifetimeAndInterlocks();
    void parameterRecoveryBindingAndDefaultCancel();
    void parameterRecoveryRejectsChangedOrArmedTarget();
    void parameterRecoveryExecutesOrderedPlanAndReports();
    void parameterRecoveryCancellationIsOwnedAndRetained();
    void remoteDataFlashBindingAndStartConsent();
    void remoteDataFlashSessionSurvivesCloseAndSaves();
    void remoteDataFlashRejectsStaleConsentAndOldStopToken();
};

void ConfigDeveloperToolsViewTest::mirrorsMissionPlannerInventory()
{
    ConfigDeveloperToolsView view;
    QCOMPARE(view.objectName(), QStringLiteral("ConfigDeveloperToolsView"));
    QCOMPARE(view.Title(), QStringLiteral("Developer Tools"));
    QCOMPARE(view.ActionCount(), 32);
    QCOMPARE(view.ImplementedActionCount(), 10);
    QVERIFY(view.Log().contains(QStringLiteral("10 of 32")));

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
    QCOMPARE(enabled, 9); // Shapefile conversion is offline; owned Cancel stays disabled while idle.
    QVERIFY(view.findChild<QPushButton *>(
        QStringLiteral("DecodeMavlinkPacketButton"))->isEnabled());
    QVERIFY(view.findChild<QPushButton *>(
        QStringLiteral("DecodeHardwareIdButton"))->isEnabled());
    QVERIFY(view.findChild<QPushButton *>(
        QStringLiteral("SplitDataFlashLogButton"))->isEnabled());
    QVERIFY(view.findChild<QPushButton *>(
        QStringLiteral("CreateDashWareCsvButton"))->isEnabled());
    QVERIFY(view.findChild<QPushButton *>(
        QStringLiteral("EmbedDefaultsInApjButton"))->isEnabled());
    QVERIFY(view.findChild<QPushButton *>(
        QStringLiteral("OrganizeLogDirectoryButton"))->isEnabled());
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
    QAction magFit(&actionSource);
    magFit.setObjectName(QStringLiteral("actionOfflineMagFit"));
    QSignalSpy magFitTriggered(&magFit, &QAction::triggered);
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
    QCOMPARE(view.ImplementedActionCount(), 14);
    QVERIFY(view.Log().contains(QStringLiteral("14 of 32")));
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
    auto *magFitButton = tool(view, "OfflineMagFitButton");
    QVERIFY(magFitButton);
    QVERIFY(magFitButton->isEnabled());
    magFitButton->click();
    QCOMPARE(magFitTriggered.count(), 1);
    magFit.setEnabled(false);
    QVERIFY(!magFitButton->isEnabled());
    magFit.setEnabled(true);
    QVERIFY(magFitButton->isEnabled());
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

    VehicleFixture fixture;
    view.setVehicleToolService(&fixture.service);
    QCOMPARE(view.ImplementedActionCount(), 21);
    DeveloperFtpStub ftp;
    view.setMavFtpDownloadServices(&ftp, &fixture.targets);
    QCOMPARE(view.ImplementedActionCount(), 22);
    ParameterRecoveryService recovery(
        &fixture.targets, &fixture.registry, &fixture.parameters,
        &fixture.commands,
        [](const SwarmVehicleInstanceLease &, QString *) { return true; });
    view.setParameterRecoveryService(&recovery);
    QCOMPARE(view.ImplementedActionCount(), 24);
    QTemporaryDir remoteDirectory;
    QVERIFY(remoteDirectory.isValid());
    fixture.transmitter.setLinkSessionEpoch(
        fixture.endpoint.linkId, fixture.session);
    RemoteDataFlashLogService remoteLog(
        &fixture.targets, &fixture.registry, &fixture.parameters,
        &fixture.transmitter, 250, 190,
        [](const SwarmVehicleInstanceLease &, QString *) { return true; });
    view.setRemoteDataFlashLogService(&remoteLog, remoteDirectory.path());
    QCOMPARE(view.ImplementedActionCount(), 26);
    view.setRemoteDataFlashLogService(nullptr, QString());
    QCOMPARE(view.ImplementedActionCount(), 24);
    view.setParameterRecoveryService(nullptr);
    QCOMPARE(view.ImplementedActionCount(), 22);
    view.setMavFtpDownloadServices(nullptr, nullptr);
    QCOMPARE(view.ImplementedActionCount(), 21);
    view.setVehicleToolService(nullptr);
    QCOMPARE(view.ImplementedActionCount(), 14);
}

void ConfigDeveloperToolsViewTest::serialBridgeSharedActionTracksAvailability()
{
    QObject source;
    QAction bridge(&source);
    bridge.setObjectName(QStringLiteral("actionMavlinkSerialTcpBridge"));
    QSignalSpy triggered(&bridge, &QAction::triggered);
    ConfigDeveloperToolsView view(&source);
    auto *button = tool(view, "MavlinkSerialTcpBridgeButton");
    QVERIFY(button && button->isEnabled());
    QCOMPARE(view.ActionCount(), 32);
    button->click();
    QCOMPARE(triggered.count(), 1);
    bridge.setEnabled(false);
    QVERIFY(!button->isEnabled());
    bridge.setEnabled(true);
    QVERIFY(button->isEnabled());
}

void ConfigDeveloperToolsViewTest::logIndexSharedActionTracksAvailability()
{
    QObject source;
    QAction index(&source);
    index.setObjectName(QStringLiteral("actionFlightLogIndex"));
    QSignalSpy triggered(&index, &QAction::triggered);
    ConfigDeveloperToolsView view(&source);
    auto *button = tool(view, "FlightLogIndexButton");
    QVERIFY(button && button->isEnabled());
    QCOMPARE(view.ActionCount(), 32);
    button->click();
    QCOMPARE(triggered.count(), 1);
    index.setEnabled(false);
    QVERIFY(!button->isEnabled());
    index.setEnabled(true);
    QVERIFY(button->isEnabled());
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
    QCOMPARE(view.ImplementedActionCount(), 17);
    QVERIFY(tool(view, "SetQnhButton")->isEnabled());
    QVERIFY(tool(view, "RebootVehicleButton")->isEnabled());
    QVERIFY(tool(view, "UpgradeBootloaderButton")->isEnabled());
    fixture.heartbeat(true);
    QTRY_VERIFY(!tool(view, "RebootVehicleButton")->isEnabled());
    QVERIFY(!tool(view, "UpgradeBootloaderButton")->isEnabled());
    QVERIFY(!tool(view, "RebootVehicleButton")->toolTip().isEmpty());
    fixture.heartbeat(false);
    QTRY_VERIFY(tool(view, "RebootVehicleButton")->isEnabled());
    fixture.registry.endLinkSession(fixture.endpoint.linkId, fixture.session);
    QTRY_VERIFY(!tool(view, "RebootVehicleButton")->isEnabled());
    view.setVehicleToolService(nullptr);
    QCOMPARE(view.ImplementedActionCount(), 10);
    QVERIFY(!tool(view, "SetQnhButton")->isEnabled());
    QVERIFY(fixture.frames.isEmpty());
}

void ConfigDeveloperToolsViewTest::cameraProbeBindingAndGateRestoration()
{
    CameraProbePageFixture fixture;
    QVERIFY(fixture.configured);
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.vehicle.service);
    QCOMPARE(view.ImplementedActionCount(), 17);
    view.setCameraProbeService(&fixture.service);
    QCOMPARE(view.ImplementedActionCount(), 18);
    view.show();

    auto *probe = tool(view, "ProbeMavlinkCameraButton");
    auto *reboot = tool(view, "RebootVehicleButton");
    QVERIFY(probe);
    QVERIFY(reboot);
    QTRY_VERIFY(probe->isEnabled());
    QTRY_VERIFY(reboot->isEnabled());

    // A pending camera consent gates every other Developer operation, and a
    // default-Cancel dismissal restores all gates without transmitting.
    probe->click();
    auto *confirm = visibleNamed<QMessageBox>(
        &view, "DeveloperCameraProbeConfirmation");
    QVERIFY(confirm);
    QVERIFY(!reboot->isEnabled());
    QCOMPARE(confirm->defaultButton(),
             confirm->button(QMessageBox::Cancel));
    confirm->button(QMessageBox::Cancel)->click();
    QVERIFY(fixture.vehicle.frames.isEmpty());
    QTRY_VERIFY(probe->isEnabled());
    QTRY_VERIFY(reboot->isEnabled());

    // Complete the real six-command service flow. Its final stateChanged()
    // follows the service finishing fence and must restore vehicle actions,
    // not only the offline-file buttons.
    probe->click();
    confirm = visibleNamed<QMessageBox>(
        &view, "DeveloperCameraProbeConfirmation");
    QVERIFY(confirm);
    confirm->button(QMessageBox::Yes)->click();
    const QList<MAV_CMD> commands = CameraProbeService::Commands();
    for (int index = 0; index < commands.size(); ++index) {
        QTRY_COMPARE(fixture.vehicle.frames.size(), index + 1);
        const mavlink_message_t message =
            fixture.vehicle.messageAt(index);
        QCOMPARE(message.msgid,
                 static_cast<quint32>(MAVLINK_MSG_ID_COMMAND_LONG));
        mavlink_command_long_t payload{};
        mavlink_msg_command_long_decode(&message, &payload);
        QCOMPARE(payload.command,
                 static_cast<quint16>(commands.at(index)));
        QCOMPARE(payload.target_component,
                 static_cast<quint8>(MAV_COMP_ID_CAMERA));
        fixture.acknowledge(commands.at(index));
    }
    QTRY_VERIFY(!fixture.service.busy());
    QTRY_VERIFY(probe->isEnabled());
    QTRY_VERIFY(reboot->isEnabled());

    // Closing requests cancellation only for this page's exact operation.
    // Its app-owned terminal history is deliberately not marked seen while
    // the page is closed, then appears exactly once after reopening.
    const int priorFrames = fixture.vehicle.frames.size();
    probe->click();
    confirm = visibleNamed<QMessageBox>(
        &view, "DeveloperCameraProbeConfirmation");
    QVERIFY(confirm);
    confirm->button(QMessageBox::Yes)->click();
    QTRY_COMPARE(fixture.vehicle.frames.size(), priorFrames + 1);
    view.close();
    fixture.acknowledge(commands.constFirst());
    QTRY_VERIFY(!fixture.service.busy());
    const QString cancelledTerminal = QStringLiteral(
        "Camera probe cancelled. Remaining requests were not sent");
    QVERIFY(!view.Log().contains(cancelledTerminal));
    view.show();
    QTRY_VERIFY(view.Log().contains(cancelledTerminal));
    QCOMPARE(view.Log().count(cancelledTerminal), 1);
    QTRY_VERIFY(reboot->isEnabled());

    view.setCameraProbeService(nullptr);
    QCOMPARE(view.ImplementedActionCount(), 17);
    QVERIFY(!probe->isEnabled());
    QVERIFY(reboot->isEnabled());
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
    QVERIFY(!tool(view, "EmbedDefaultsInApjButton")->isEnabled());
    QVERIFY(!tool(view, "OrganizeLogDirectoryButton")->isEnabled());
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

void ConfigDeveloperToolsViewTest::upgradeBootloaderRequiresTwoConfirmations()
{
    VehicleFixture fixture;
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.service);
    view.show();
    auto *button = tool(view, "UpgradeBootloaderButton");
    QVERIFY(button->isEnabled());

    button->click();
    auto *sourceConfirm = visibleNamed<QMessageBox>(
        &view, "DeveloperUpgradeBootloaderSourceConfirmation");
    QVERIFY(sourceConfirm);
    QCOMPARE(sourceConfirm->defaultButton(),
             sourceConfirm->button(QMessageBox::Cancel));
    QCOMPARE(sourceConfirm->escapeButton(),
             sourceConfirm->button(QMessageBox::Cancel));
    QVERIFY(sourceConfirm->text().contains(
        QStringLiteral("link 7, system 42, component 1")));
    QVERIFY(sourceConfirm->text().contains(QStringLiteral("Bench vehicle")));
    QVERIFY(sourceConfirm->text().contains(
        QStringLiteral("does not select or download")));
    sourceConfirm->button(QMessageBox::Cancel)->click();
    QVERIFY(fixture.frames.isEmpty());
    QVERIFY(!fixture.service.busy());

    button->click();
    sourceConfirm = visibleNamed<QMessageBox>(
        &view, "DeveloperUpgradeBootloaderSourceConfirmation");
    QVERIFY(sourceConfirm);
    sourceConfirm->button(QMessageBox::Yes)->click();
    QVERIFY(fixture.frames.isEmpty()); // First consent never transmits.
    auto *flashConfirm = visibleNamed<QMessageBox>(
        &view, "DeveloperUpgradeBootloaderFlashConfirmation");
    QVERIFY(flashConfirm);
    QCOMPARE(flashConfirm->defaultButton(),
             flashConfirm->button(QMessageBox::Cancel));
    QCOMPARE(flashConfirm->escapeButton(),
             flashConfirm->button(QMessageBox::Cancel));
    QVERIFY(flashConfirm->text().contains(
        QStringLiteral("link 7, system 42, component 1")));
    QVERIFY(flashConfirm->text().contains(QStringLiteral("Bench vehicle")));
    QVERIFY(flashConfirm->text().contains(
        QStringLiteral("Power loss or interruption can brick")));
    QVERIFY(flashConfirm->text().contains(
        QStringLiteral("at least five minutes")));
    QVERIFY(flashConfirm->text().contains(
        QStringLiteral("Heartbeats and telemetry may pause")));
    QVERIFY(flashConfirm->text().contains(
        QStringLiteral("never retries")));
    QVERIFY(flashConfirm->text().contains(
        QStringLiteral("do not automatically retry or power-cycle")));
    flashConfirm->button(QMessageBox::Cancel)->click();
    QVERIFY(fixture.frames.isEmpty());
    QVERIFY(!fixture.service.busy());

    button->click();
    sourceConfirm = visibleNamed<QMessageBox>(
        &view, "DeveloperUpgradeBootloaderSourceConfirmation");
    QVERIFY(sourceConfirm);
    sourceConfirm->button(QMessageBox::Yes)->click();
    flashConfirm = visibleNamed<QMessageBox>(
        &view, "DeveloperUpgradeBootloaderFlashConfirmation");
    QVERIFY(flashConfirm);
    flashConfirm->button(QMessageBox::Yes)->click();
    QCOMPARE(fixture.frames.size(), 1);
    QVERIFY(fixture.service.busy());
    const mavlink_message_t message = fixture.lastMessage();
    QCOMPARE(message.msgid,
             static_cast<quint32>(MAVLINK_MSG_ID_COMMAND_LONG));
    mavlink_command_long_t sent{};
    mavlink_msg_command_long_decode(&message, &sent);
    QCOMPARE(sent.command, quint16(MAV_CMD_FLASH_BOOTLOADER));
    QCOMPARE(sent.target_system, quint8(42));
    QCOMPARE(sent.target_component, quint8(1));
    QCOMPARE(sent.param1, 0.0f);
    QCOMPARE(sent.param2, 0.0f);
    QCOMPARE(sent.param3, 0.0f);
    QCOMPARE(sent.param4, 0.0f);
    QCOMPARE(sent.param5, 290876.0f);
    QCOMPARE(sent.param6, 0.0f);
    QCOMPARE(sent.param7, 0.0f);
    view.close();
    QVERIFY(fixture.service.busy());
    fixture.acknowledge(MAV_CMD_FLASH_BOOTLOADER);
    QVERIFY(!fixture.service.busy());
    view.show();
    QTRY_VERIFY_WITH_TIMEOUT(
        view.Log().contains(fixture.service.lastReport().description), 2000);
}

void ConfigDeveloperToolsViewTest::upgradeBootloaderRejectsStaleSecondConsent()
{
    VehicleFixture fixture;
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.service);
    view.show();
    auto *button = tool(view, "UpgradeBootloaderButton");

    // Becoming armed while the first warning is open prevents advancement to
    // the flash warning and cannot transmit.
    button->click();
    auto *sourceConfirm = visibleNamed<QMessageBox>(
        &view, "DeveloperUpgradeBootloaderSourceConfirmation");
    QVERIFY(sourceConfirm);
    fixture.heartbeat(true);
    sourceConfirm->button(QMessageBox::Yes)->click();
    QVERIFY(!visibleNamed<QMessageBox>(
        &view, "DeveloperUpgradeBootloaderFlashConfirmation"));
    QVERIFY(fixture.frames.isEmpty());
    fixture.heartbeat(false);
    QTRY_VERIFY(button->isEnabled());

    // Switching away and back changes the immutable target generation. The
    // old second-stage consent must not retarget to the visually same drone.
    button->click();
    sourceConfirm = visibleNamed<QMessageBox>(
        &view, "DeveloperUpgradeBootloaderSourceConfirmation");
    QVERIFY(sourceConfirm);
    sourceConfirm->button(QMessageBox::Yes)->click();
    auto *flashConfirm = visibleNamed<QMessageBox>(
        &view, "DeveloperUpgradeBootloaderFlashConfirmation");
    QVERIFY(flashConfirm);
    VehicleEndpoint other = fixture.endpoint;
    other.linkId = 8;
    QVERIFY(fixture.targets.observeEndpoint(other));
    QVERIFY(fixture.targets.selectTarget(8, 42, 1));
    QVERIFY(fixture.targets.selectTarget(7, 42, 1));
    // Real telemetry re-establishes freshness for the new target generation.
    // The old consent must still fail despite a fresh disarmed heartbeat.
    fixture.heartbeat(false);
    flashConfirm->button(QMessageBox::Yes)->click();
    QVERIFY(fixture.frames.isEmpty());
    QVERIFY(view.Log().contains(QStringLiteral("cancelled")));

    // Closing either stage invalidates its revision. Delayed acceptance is
    // ignored and cannot send after the page is reopened.
    QTRY_VERIFY(button->isEnabled());
    button->click();
    QPointer<QMessageBox> closingSource = visibleNamed<QMessageBox>(
        &view, "DeveloperUpgradeBootloaderSourceConfirmation");
    QVERIFY(closingSource);
    view.close();
    if (closingSource)
        closingSource->done(QMessageBox::Yes);
    QVERIFY(fixture.frames.isEmpty());
    view.show();
    QTRY_VERIFY(button->isEnabled());
    button->click();
    sourceConfirm = visibleNamed<QMessageBox>(
        &view, "DeveloperUpgradeBootloaderSourceConfirmation");
    QVERIFY(sourceConfirm);
    sourceConfirm->button(QMessageBox::Yes)->click();
    QPointer<QMessageBox> closingFlash = visibleNamed<QMessageBox>(
        &view, "DeveloperUpgradeBootloaderFlashConfirmation");
    QVERIFY(closingFlash);
    view.close();
    if (closingFlash)
        closingFlash->done(QMessageBox::Yes);
    QVERIFY(fixture.frames.isEmpty());

    // prepare() itself emits stateChanged. Closing from that callback must
    // invalidate the pre-prepare prompt revision instead of opening a warning
    // on the now-hidden page.
    view.show();
    QTRY_VERIFY(button->isEnabled());
    bool closeDuringPrepare = true;
    const auto connection = connect(
        &fixture.service, &DeveloperVehicleToolService::stateChanged,
        &view, [&view, &closeDuringPrepare]() {
        if (closeDuringPrepare) {
            closeDuringPrepare = false;
            view.close();
        }
    });
    button->click();
    disconnect(connection);
    QVERIFY(!closeDuringPrepare);
    QVERIFY(!visibleNamed<QMessageBox>(
        &view, "DeveloperUpgradeBootloaderSourceConfirmation"));
    QVERIFY(!visibleNamed<QMessageBox>(
        &view, "DeveloperUpgradeBootloaderFlashConfirmation"));
    QVERIFY(fixture.frames.isEmpty());
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
        QVERIFY(!tool(view, "EmbedDefaultsInApjButton")->isEnabled());
        QVERIFY(!tool(view, "OrganizeLogDirectoryButton")->isEnabled());
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

void ConfigDeveloperToolsViewTest::splitDialogsAreDefaultCancelAndNonDestructive()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.log"));
    QVERIFY(writeFixture(input, splittableAsciiLog()));
    ConfigDeveloperToolsView view;
    view.show();
    auto *button = tool(view, "SplitDataFlashLogButton");
    QVERIFY(button->isEnabled());

    button->click();
    auto *picker = view.findChild<QFileDialog *>(
        QStringLiteral("DeveloperSplitInputDialog"));
    QVERIFY(picker);
    QVERIFY(picker->testOption(QFileDialog::DontUseNativeDialog));
    QCOMPARE(picker->fileMode(), QFileDialog::ExistingFile);
    QVERIFY(picker->nameFilters().join(QLatin1Char(' '))
                .contains(QStringLiteral("*.bin")));
    QVERIFY(!button->isEnabled());
    picker->reject();
    QVERIFY(button->isEnabled());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    button->click();
    picker = view.findChild<QFileDialog *>(
        QStringLiteral("DeveloperSplitInputDialog"));
    QVERIFY(picker);
    picker->selectFile(input);
    QVERIFY(QMetaObject::invokeMethod(picker, "accept",
                                      Qt::DirectConnection));
    auto *count = view.findChild<QInputDialog *>(
        QStringLiteral("DeveloperSplitCountDialog"));
    QVERIFY(count);
    QCOMPARE(count->inputMode(), QInputDialog::IntInput);
    QCOMPARE(count->intMinimum(), 2);
    QCOMPARE(count->intMaximum(), 1000);
    QCOMPARE(count->intValue(), 10);
    count->reject();
    QVERIFY(button->isEnabled());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    button->click();
    picker = view.findChild<QFileDialog *>(
        QStringLiteral("DeveloperSplitInputDialog"));
    QVERIFY(picker);
    picker->selectFile(input);
    QVERIFY(QMetaObject::invokeMethod(picker, "accept",
                                      Qt::DirectConnection));
    count = view.findChild<QInputDialog *>(
        QStringLiteral("DeveloperSplitCountDialog"));
    QVERIFY(count);
    count->setIntValue(3);
    count->accept();
    auto *confirm = view.findChild<QMessageBox *>(
        QStringLiteral("DeveloperSplitConfirmDialog"));
    QVERIFY(confirm);
    QCOMPARE(confirm->textFormat(), Qt::PlainText);
    QCOMPARE(confirm->defaultButton(),
             confirm->button(QMessageBox::Cancel));
    QCOMPARE(confirm->escapeButton(),
             confirm->button(QMessageBox::Cancel));
    const QStringList outputs = DataFlashLogSplitter::OutputPaths(input, 3);
    QCOMPARE(outputs.size(), 3);
    QVERIFY(confirm->text().contains(outputs.first()));
    QVERIFY(confirm->text().contains(outputs.last()));
    QVERIFY(confirm->text().contains(QStringLiteral("never overwritten")));
    QVERIFY(confirm->text().contains(QStringLiteral("not group-atomic")));
    QVERIFY(confirm->text().contains(QStringLiteral("Complete records")));
    confirm->button(QMessageBox::Cancel)->click();
    QVERIFY(button->isEnabled());
    QVERIFY(!view.findChild<QProgressDialog *>(
        QStringLiteral("DeveloperSplitProgressDialog")));
    for (const QString &path : outputs)
        QVERIFY(!QFile::exists(path));
}

void ConfigDeveloperToolsViewTest::splitCompletesOfflineAndReportsCounts()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.log"));
    QVERIFY(writeFixture(input, splittableAsciiLog(6)));
    const QStringList outputs = DataFlashLogSplitter::OutputPaths(input, 2);
    QCOMPARE(outputs.size(), 2);
    ConfigDeveloperToolsView view;
    view.show();
    view.SplitDataFlashLog(input, 2);
    auto *button = tool(view, "SplitDataFlashLogButton");
    QVERIFY(!button->isEnabled());
    QVERIFY(!tool(view, "ExtractGpsCorrectionsButton")->isEnabled());
    QVERIFY(view.findChild<QProgressDialog *>(
        QStringLiteral("DeveloperSplitProgressDialog")));
    QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 5000);
    qint64 publishedBytes = 0;
    for (const QString &path : outputs) {
        QVERIFY2(QFile::exists(path), qPrintable(path));
        const QByteArray part = readFixture(path);
        QVERIFY(part.contains("FMT,128,89,FMT"));
        QVERIFY(part.contains("FMT,150,11,DUMY"));
        publishedBytes += part.size();
    }
    QVERIFY(view.Log().contains(QStringLiteral("split completed: 2 files")));
    QVERIFY(view.Log().contains(QStringLiteral("8 records (6 data records)")));
    QVERIFY(view.Log().contains(QString::number(publishedBytes)));
    QVERIFY(view.Log().contains(QStringLiteral("not group-atomic")));
}

void ConfigDeveloperToolsViewTest::splitRefusesExistingOutputs()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.log"));
    QVERIFY(writeFixture(input, splittableAsciiLog()));
    const QStringList outputs = DataFlashLogSplitter::OutputPaths(input, 2);
    QCOMPARE(outputs.size(), 2);
    const QByteArray sentinel("do not overwrite");
    QVERIFY(writeFixture(outputs.first(), sentinel));
    ConfigDeveloperToolsView view;
    view.show();
    view.SplitDataFlashLog(input, 2);
    QTRY_VERIFY_WITH_TIMEOUT(
        tool(view, "SplitDataFlashLogButton")->isEnabled(), 5000);
    QCOMPARE(readFixture(outputs.first()), sentinel);
    QVERIFY(!QFile::exists(outputs.last()));
    QVERIFY(view.Log().contains(QStringLiteral("split failed")));
    QVERIFY(!view.Log().contains(QStringLiteral("split completed")));
}

void ConfigDeveloperToolsViewTest::splitCancellationAndLifetime_data()
{
    QTest::addColumn<QString>("operation");
    QTest::newRow("cancel") << QStringLiteral("cancel");
    QTest::newRow("close") << QStringLiteral("close");
    QTest::newRow("destroy") << QStringLiteral("destroy");
}

void ConfigDeveloperToolsViewTest::splitCancellationAndLifetime()
{
    QFETCH(QString, operation);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.log"));
    QVERIFY(writeFixture(input, splittableAsciiLog(20)));
    const QStringList outputs = DataFlashLogSplitter::OutputPaths(input, 3);
    QPointer<ConfigDeveloperToolsView> view =
        new ConfigDeveloperToolsView;
    view->show();
    QString logAtClose;
    {
        PausedGlobalPool paused;
        QVERIFY(paused.ready);
        view->SplitDataFlashLog(input, 3);
        view->SplitDataFlashLog(input, 4);
        QVERIFY(view->Log().contains(QStringLiteral("already active")));
        auto *progress = view->findChild<QProgressDialog *>(
            QStringLiteral("DeveloperSplitProgressDialog"));
        QVERIFY(progress);
        if (operation == QStringLiteral("destroy")) {
            delete view.data();
        } else if (operation == QStringLiteral("close")) {
            logAtClose = view->Log();
            view->close();
        } else {
            auto *cancel = progress->findChild<QPushButton *>();
            QVERIFY(cancel);
            cancel->click();
        }
    }
    QCoreApplication::processEvents();
    for (const QString &path : outputs)
        QVERIFY(!QFile::exists(path));
    if (operation == QStringLiteral("destroy")) {
        QVERIFY(view.isNull());
    } else if (operation == QStringLiteral("close")) {
        QCOMPARE(view->Log(), logAtClose);
        view->show();
        QTRY_VERIFY(tool(*view, "SplitDataFlashLogButton")->isEnabled());
        delete view.data();
    } else {
        QTRY_VERIFY(view->Log().contains(QStringLiteral("split cancelled")));
        QVERIFY(tool(*view, "SplitDataFlashLogButton")->isEnabled());
        delete view.data();
    }
}

void ConfigDeveloperToolsViewTest::splitAndOtherOperationsInterlock()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.log"));
    const QString correctionInput =
        directory.filePath(QStringLiteral("flight.tlog"));
    const QString correctionOutput =
        directory.filePath(QStringLiteral("corrections.dat"));
    QVERIFY(writeFixture(input, splittableAsciiLog(20)));
    QVERIFY(writeFixture(correctionInput, correctionLog()));
    VehicleFixture fixture;
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.service);
    view.show();

    tool(view, "RebootVehicleButton")->click();
    auto *consent = confirmation(view);
    QVERIFY(consent);
    QVERIFY(!tool(view, "SplitDataFlashLogButton")->isEnabled());
    view.SplitDataFlashLog(input, 2);
    QVERIFY(!view.findChild<QProgressDialog *>(
        QStringLiteral("DeveloperSplitProgressDialog")));
    consent->button(QMessageBox::Cancel)->click();

    {
        PausedGlobalPool paused;
        QVERIFY(paused.ready);
        view.SplitDataFlashLog(input, 2);
        QVERIFY(!tool(view, "ExtractGpsCorrectionsButton")->isEnabled());
        QVERIFY(!tool(view, "EmbedDefaultsInApjButton")->isEnabled());
        QVERIFY(!tool(view, "OrganizeLogDirectoryButton")->isEnabled());
        QVERIFY(!tool(view, "RebootVehicleButton")->isEnabled());
        view.ExtractGpsCorrections(correctionInput, correctionOutput);
        QVERIFY(!view.findChild<QProgressDialog *>(
            QStringLiteral("DeveloperGpsProgressDialog")));
        tool(view, "RebootVehicleButton")->click();
        QVERIFY(!confirmation(view));
        QVERIFY(fixture.frames.isEmpty());
        auto *progress = view.findChild<QProgressDialog *>(
            QStringLiteral("DeveloperSplitProgressDialog"));
        QVERIFY(progress);
        progress->findChild<QPushButton *>()->click();
    }
    QTRY_VERIFY(tool(view, "SplitDataFlashLogButton")->isEnabled());
    QTRY_VERIFY(tool(view, "ExtractGpsCorrectionsButton")->isEnabled());
    QTRY_VERIFY(tool(view, "RebootVehicleButton")->isEnabled());
    QVERIFY(!QFile::exists(correctionOutput));
    for (const QString &path : DataFlashLogSplitter::OutputPaths(input, 2))
        QVERIFY(!QFile::exists(path));
}

void ConfigDeveloperToolsViewTest::dashWareDialogsAreCancellableAndUseMpDefaults()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.log"));
    const QString output = directory.filePath(
        QStringLiteral("flight-dashware.csv"));
    QVERIFY(writeFixture(input, dashWareAsciiLog()));
    ConfigDeveloperToolsView view;
    view.show();
    auto *button = tool(view, "CreateDashWareCsvButton");
    QVERIFY(button->isEnabled());

    button->click();
    auto *picker = view.findChild<QFileDialog *>(
        QStringLiteral("DeveloperDashWareInputDialog"));
    QVERIFY(picker);
    QVERIFY(picker->testOption(QFileDialog::DontUseNativeDialog));
    QCOMPARE(picker->fileMode(), QFileDialog::ExistingFile);
    QVERIFY(picker->nameFilters().join(QLatin1Char(' '))
                .contains(QStringLiteral("*.log")));
    QVERIFY(!button->isEnabled());
    picker->reject();
    QVERIFY(button->isEnabled());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    button->click();
    picker = view.findChild<QFileDialog *>(
        QStringLiteral("DeveloperDashWareInputDialog"));
    QVERIFY(picker);
    picker->selectFile(input);
    QVERIFY(QMetaObject::invokeMethod(picker, "accept",
                                      Qt::DirectConnection));
    auto *types = view.findChild<QInputDialog *>(
        QStringLiteral("DeveloperDashWareTypesDialog"));
    QVERIFY(types);
    QCOMPARE(types->inputMode(), QInputDialog::TextInput);
    QCOMPARE(types->textValue(),
             QStringLiteral("GPS;ATT;NTUN;CTUN;MODE;BAT"));
    QVERIFY(types->labelText().contains(QStringLiteral("empty includes all")));
    types->reject();
    QVERIFY(button->isEnabled());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    button->click();
    picker = view.findChild<QFileDialog *>(
        QStringLiteral("DeveloperDashWareInputDialog"));
    QVERIFY(picker);
    picker->selectFile(input);
    QVERIFY(QMetaObject::invokeMethod(picker, "accept",
                                      Qt::DirectConnection));
    types = view.findChild<QInputDialog *>(
        QStringLiteral("DeveloperDashWareTypesDialog"));
    QVERIFY(types);
    types->setTextValue(QStringLiteral(" gps ; ATT ; gps ;; "));
    types->accept();
    auto *save = view.findChild<QFileDialog *>(
        QStringLiteral("DeveloperDashWareOutputDialog"));
    QVERIFY(save);
    QCOMPARE(save->acceptMode(), QFileDialog::AcceptSave);
    QCOMPARE(save->fileMode(), QFileDialog::AnyFile);
    QVERIFY(!save->testOption(QFileDialog::DontConfirmOverwrite));
    QCOMPARE(save->defaultSuffix(), QStringLiteral("csv"));
    QCOMPARE(save->selectedFiles(), QStringList{output});
    save->reject();
    QVERIFY(button->isEnabled());
    QVERIFY(!QFile::exists(output));
    QVERIFY(!view.findChild<QProgressDialog *>(
        QStringLiteral("DeveloperDashWareProgressDialog")));
}

void ConfigDeveloperToolsViewTest::dashWareExportCompletesOfflineAndReportsCounts()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.log"));
    const QString output = directory.filePath(QStringLiteral("dashware.csv"));
    QVERIFY(writeFixture(input, dashWareAsciiLog()));
    ConfigDeveloperToolsView view;
    view.show();
    view.ExportDashWareCsv(input, output,
                           {QStringLiteral(" gps "), QStringLiteral("GPS")});
    auto *button = tool(view, "CreateDashWareCsvButton");
    QVERIFY(!button->isEnabled());
    QVERIFY(!tool(view, "ExtractGpsCorrectionsButton")->isEnabled());
    QVERIFY(!tool(view, "SplitDataFlashLogButton")->isEnabled());
    QVERIFY(view.findChild<QProgressDialog *>(
        QStringLiteral("DeveloperDashWareProgressDialog")));
    QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 5000);
    QVERIFY(QFile::exists(output));
    const QByteArray csv = readFixture(output);
    QCOMPARE(csv, QByteArray(
        "GLOBAL_TimeMS,GPS_TimeUS,GPS_Lat,GPS_Lng,\n"
        "1000,1000000,1.5,2.5,\n"
        "2000,2000000,3.5,4.5,\n"));
    QVERIFY(view.Log().contains(QStringLiteral("(GPS)")));
    QVERIFY(view.Log().contains(QStringLiteral("export completed: 2 rows")));
    QVERIFY(view.Log().contains(QStringLiteral("4 columns")));
    QVERIFY(view.Log().contains(QString::number(csv.size())));
}

void ConfigDeveloperToolsViewTest::dashWareFailurePreservesExistingOutput()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("broken.log"));
    const QString output = directory.filePath(QStringLiteral("dashware.csv"));
    const QByteArray sentinel("existing csv must survive");
    QVERIFY(!QFile::exists(input));
    QVERIFY(writeFixture(output, sentinel));
    ConfigDeveloperToolsView view;
    view.show();
    view.ExportDashWareCsv(input, output, {});
    QTRY_VERIFY_WITH_TIMEOUT(
        tool(view, "CreateDashWareCsvButton")->isEnabled(), 5000);
    QCOMPARE(readFixture(output), sentinel);
    QVERIFY(view.Log().contains(QStringLiteral("export failed")));
    QVERIFY(!view.Log().contains(QStringLiteral("export completed")));
}

void ConfigDeveloperToolsViewTest::dashWareCancellationAndLifetime_data()
{
    QTest::addColumn<QString>("operation");
    QTest::newRow("cancel") << QStringLiteral("cancel");
    QTest::newRow("close") << QStringLiteral("close");
    QTest::newRow("destroy") << QStringLiteral("destroy");
}

void ConfigDeveloperToolsViewTest::dashWareCancellationAndLifetime()
{
    QFETCH(QString, operation);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.log"));
    const QString output = directory.filePath(QStringLiteral("dashware.csv"));
    const QString secondOutput = directory.filePath(
        QStringLiteral("must-not-exist.csv"));
    const QByteArray sentinel("old output");
    QVERIFY(writeFixture(input, dashWareAsciiLog(20)));
    QVERIFY(writeFixture(output, sentinel));
    QPointer<ConfigDeveloperToolsView> view =
        new ConfigDeveloperToolsView;
    view->show();
    QString logAtClose;
    {
        PausedGlobalPool paused;
        QVERIFY(paused.ready);
        view->ExportDashWareCsv(input, output, {QStringLiteral("GPS")});
        view->ExportDashWareCsv(input, secondOutput,
                                {QStringLiteral("GPS")});
        QVERIFY(view->Log().contains(QStringLiteral("already active")));
        auto *progress = view->findChild<QProgressDialog *>(
            QStringLiteral("DeveloperDashWareProgressDialog"));
        QVERIFY(progress);
        if (operation == QStringLiteral("destroy")) {
            delete view.data();
        } else if (operation == QStringLiteral("close")) {
            logAtClose = view->Log();
            view->close();
        } else {
            auto *cancel = progress->findChild<QPushButton *>();
            QVERIFY(cancel);
            cancel->click();
        }
    }
    QCoreApplication::processEvents();
    QCOMPARE(readFixture(output), sentinel);
    QVERIFY(!QFile::exists(secondOutput));
    if (operation == QStringLiteral("destroy")) {
        QVERIFY(view.isNull());
    } else if (operation == QStringLiteral("close")) {
        QCOMPARE(view->Log(), logAtClose);
        view->show();
        QTRY_VERIFY(tool(*view, "CreateDashWareCsvButton")->isEnabled());
        delete view.data();
    } else {
        QTRY_VERIFY(view->Log().contains(QStringLiteral("export cancelled")));
        QVERIFY(tool(*view, "CreateDashWareCsvButton")->isEnabled());
        delete view.data();
    }
}

void ConfigDeveloperToolsViewTest::dashWareAndOtherOperationsInterlock()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString input = directory.filePath(QStringLiteral("flight.log"));
    const QString output = directory.filePath(QStringLiteral("dashware.csv"));
    const QString corrections = directory.filePath(QStringLiteral("flight.tlog"));
    const QString correctionOutput = directory.filePath(
        QStringLiteral("corrections.dat"));
    QVERIFY(writeFixture(input, dashWareAsciiLog(20)));
    QVERIFY(writeFixture(corrections, correctionLog()));
    VehicleFixture fixture;
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.service);
    view.show();

    tool(view, "RebootVehicleButton")->click();
    auto *consent = confirmation(view);
    QVERIFY(consent);
    QVERIFY(!tool(view, "CreateDashWareCsvButton")->isEnabled());
    view.ExportDashWareCsv(input, output, {QStringLiteral("GPS")});
    QVERIFY(!view.findChild<QProgressDialog *>(
        QStringLiteral("DeveloperDashWareProgressDialog")));
    consent->button(QMessageBox::Cancel)->click();

    {
        PausedGlobalPool paused;
        QVERIFY(paused.ready);
        view.ExportDashWareCsv(input, output, {QStringLiteral("GPS")});
        QVERIFY(!tool(view, "ExtractGpsCorrectionsButton")->isEnabled());
        QVERIFY(!tool(view, "SplitDataFlashLogButton")->isEnabled());
        QVERIFY(!tool(view, "EmbedDefaultsInApjButton")->isEnabled());
        QVERIFY(!tool(view, "OrganizeLogDirectoryButton")->isEnabled());
        QVERIFY(!tool(view, "RebootVehicleButton")->isEnabled());
        view.ExtractGpsCorrections(corrections, correctionOutput);
        view.SplitDataFlashLog(input, 2);
        QVERIFY(!view.findChild<QProgressDialog *>(
            QStringLiteral("DeveloperGpsProgressDialog")));
        QVERIFY(!view.findChild<QProgressDialog *>(
            QStringLiteral("DeveloperSplitProgressDialog")));
        tool(view, "RebootVehicleButton")->click();
        QVERIFY(!confirmation(view));
        QVERIFY(fixture.frames.isEmpty());
        auto *progress = view.findChild<QProgressDialog *>(
            QStringLiteral("DeveloperDashWareProgressDialog"));
        QVERIFY(progress);
        progress->findChild<QPushButton *>()->click();
    }
    QTRY_VERIFY(tool(view, "CreateDashWareCsvButton")->isEnabled());
    QTRY_VERIFY(tool(view, "ExtractGpsCorrectionsButton")->isEnabled());
    QTRY_VERIFY(tool(view, "SplitDataFlashLogButton")->isEnabled());
    QTRY_VERIFY(tool(view, "RebootVehicleButton")->isEnabled());
    QVERIFY(!QFile::exists(output));
    QVERIFY(!QFile::exists(correctionOutput));
}

void ConfigDeveloperToolsViewTest::mavFtpInjectionAndSharedOperationGate()
{
    QTemporaryDir dir;
    const QString input = dir.filePath("flight.log");
    const QString output = dir.filePath("export.csv");
    QVERIFY(writeFixture(input, dashWareAsciiLog(20)));
    VehicleFixture fixture;
    DeveloperFtpStub ftp;
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.service);
    view.setMavFtpDownloadServices(&ftp, &fixture.targets);
    view.show();
    QCOMPARE(view.ActionCount(), 32);
    QCOMPARE(view.ImplementedActionCount(), 18); // Ten offline + seven vehicle + FTP.
    auto *button = tool(view, "DownloadMavftpFileButton");
    QVERIFY(button->isEnabled());
    button->click();
    auto *path = view.findChild<QInputDialog *>("DeveloperMavFtpPathDialog");
    QVERIFY(path);
    QCOMPARE(path->textValue(), QString("@SYS/threads.txt"));
    QVERIFY(!button->isEnabled());
    QVERIFY(!tool(view, "CreateDashWareCsvButton")->isEnabled());
    QVERIFY(!tool(view, "SplitDataFlashLogButton")->isEnabled());
    QVERIFY(!tool(view, "ExtractGpsCorrectionsButton")->isEnabled());
    QVERIFY(!tool(view, "EmbedDefaultsInApjButton")->isEnabled());
    QVERIFY(!tool(view, "OrganizeLogDirectoryButton")->isEnabled());
    QVERIFY(!tool(view, "RebootVehicleButton")->isEnabled());
    view.ExportDashWareCsv(input, output, {"GPS"});
    view.SplitDataFlashLog(input, 2);
    view.ExtractGpsCorrections(input, output);
    QVERIFY(!view.findChild<QProgressDialog *>("DeveloperDashWareProgressDialog"));
    QVERIFY(!view.findChild<QProgressDialog *>("DeveloperSplitProgressDialog"));
    QVERIFY(!view.findChild<QProgressDialog *>("DeveloperGpsProgressDialog"));
    QVERIFY(fixture.frames.isEmpty());
    path->reject();
    QTRY_VERIFY(button->isEnabled());
    QTRY_VERIFY(tool(view, "RebootVehicleButton")->isEnabled());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    {
        PausedGlobalPool paused;
        QVERIFY(paused.ready);
        view.ExportDashWareCsv(input, output, {"GPS"});
        QVERIFY(!button->isEnabled());
        button->click();
        QVERIFY(!view.findChild<QInputDialog *>("DeveloperMavFtpPathDialog"));
        auto *progress = view.findChild<QProgressDialog *>("DeveloperDashWareProgressDialog");
        QVERIFY(progress);
        progress->findChild<QPushButton *>()->click();
    }
    QTRY_VERIFY(button->isEnabled());
    QVERIFY(!QFile::exists(output));
    ftp.setSharedBusy(true);
    QVERIFY(!button->isEnabled());
    QVERIFY(!tool(view, "CreateDashWareCsvButton")->isEnabled());
    view.setMavFtpDownloadServices(nullptr, nullptr);
    QCOMPARE(view.ImplementedActionCount(), 17);
    QVERIFY(!button->isEnabled());
    QCOMPARE(ftp.cancelCalls, 0); // Never cancels the browser's shared work.
}

void ConfigDeveloperToolsViewTest::mavFtpServiceRemovalDisablesAction()
{
    VehicleTargetManager targets;
    ConfigDeveloperToolsView view;
    auto *ftp = new DeveloperFtpStub;
    view.setMavFtpDownloadServices(ftp, &targets);
    QCOMPARE(view.ImplementedActionCount(), 11);
    auto *button = tool(view, "DownloadMavftpFileButton");
    QVERIFY(button->isEnabled());
    button->click(); // A disconnected tool reports the missing target, no prompt.
    QVERIFY(!view.findChild<QInputDialog *>("DeveloperMavFtpPathDialog"));
    delete ftp;
    QCOMPARE(view.ImplementedActionCount(), 10);
    QVERIFY(!button->isEnabled());
    QVERIFY(!button->toolTip().isEmpty());
}

void ConfigDeveloperToolsViewTest::apjDialogsAreDefaultCancelAndPreserveExistingOutput()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString firmware = dir.filePath(QStringLiteral("test firmware.apj"));
    const QString parameters = dir.filePath(QStringLiteral("defaults.param"));
    const QString output = firmware + QStringLiteral("new.apj");
    QVERIFY(writeFixture(firmware, embeddableApj(false, true)));
    QVERIFY(writeFixture(parameters, QByteArrayLiteral("A=1\r\n")));
    QVERIFY(writeFixture(output, QByteArrayLiteral("keep old output")));

    ConfigDeveloperToolsView view;
    view.show();
    auto *button = tool(view, "EmbedDefaultsInApjButton");
    QVERIFY(button->isEnabled());
    button->click();
    auto *firmwareDialog = view.findChild<QFileDialog *>(
        QStringLiteral("DeveloperApjFirmwareDialog"));
    QVERIFY(firmwareDialog);
    QVERIFY(firmwareDialog->nameFilters().join(QLatin1Char(';'))
                .contains(QStringLiteral("*.apj")));
    firmwareDialog->selectFile(firmware);
    QVERIFY(QMetaObject::invokeMethod(
        firmwareDialog, "accept", Qt::DirectConnection));

    auto *defaultsDialog = view.findChild<QFileDialog *>(
        QStringLiteral("DeveloperApjDefaultsDialog"));
    QVERIFY(defaultsDialog);
    const QString defaultsFilters =
        defaultsDialog->nameFilters().join(QLatin1Char(';'));
    QVERIFY(defaultsFilters.contains(QStringLiteral("*.param")));
    QVERIFY(defaultsFilters.contains(QStringLiteral("*.parm")));
    defaultsDialog->selectFile(parameters);
    QVERIFY(QMetaObject::invokeMethod(
        defaultsDialog, "accept", Qt::DirectConnection));

    auto *confirm = view.findChild<QMessageBox *>(
        QStringLiteral("DeveloperApjOverwriteConfirmDialog"));
    QVERIFY(confirm);
    QCOMPARE(static_cast<QAbstractButton *>(confirm->defaultButton()),
             confirm->button(QMessageBox::Cancel));
    QVERIFY(confirm->text().contains(output));
    QVERIFY(confirm->text().contains(QStringLiteral("not uploaded or flashed")));
    confirm->button(QMessageBox::Cancel)->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(readFixture(output), QByteArrayLiteral("keep old output"));
    QVERIFY(button->isEnabled());

    button->click();
    firmwareDialog = view.findChild<QFileDialog *>(
        QStringLiteral("DeveloperApjFirmwareDialog"));
    QVERIFY(firmwareDialog);
    firmwareDialog->selectFile(firmware);
    QVERIFY(QMetaObject::invokeMethod(
        firmwareDialog, "accept", Qt::DirectConnection));
    defaultsDialog = view.findChild<QFileDialog *>(
        QStringLiteral("DeveloperApjDefaultsDialog"));
    QVERIFY(defaultsDialog);
    defaultsDialog->selectFile(parameters);
    QVERIFY(QMetaObject::invokeMethod(
        defaultsDialog, "accept", Qt::DirectConnection));
    confirm = view.findChild<QMessageBox *>(
        QStringLiteral("DeveloperApjOverwriteConfirmDialog"));
    QVERIFY(confirm);
    confirm->button(QMessageBox::Yes)->click();
    QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 5000);
    QVERIFY(readFixture(output) != QByteArrayLiteral("keep old output"));
    QVERIFY(view.Log().contains(QStringLiteral("APJ defaults embedding completed")));
    QVERIFY(view.Log().contains(QStringLiteral("not uploaded or flashed")));
}

void ConfigDeveloperToolsViewTest::apjEmbeddingCompletesAndSignedImagesFailClosed()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString firmware = dir.filePath(QStringLiteral("board.apj"));
    const QString parameters = dir.filePath(QStringLiteral("defaults.parm"));
    const QString output = firmware + QStringLiteral("new.apj");
    QVERIFY(writeFixture(firmware, embeddableApj(false, true)));
    QVERIFY(writeFixture(parameters, QByteArrayLiteral("A=1\r\nB=2\n")));

    ConfigDeveloperToolsView view;
    view.EmbedDefaultsInApj(firmware, parameters);
    QTRY_VERIFY_WITH_TIMEOUT(tool(view, "EmbedDefaultsInApjButton")->isEnabled(),
                             5000);
    QVERIFY(QFile::exists(output));
    const QJsonDocument document = QJsonDocument::fromJson(readFixture(output));
    QVERIFY(document.isObject());
    const QJsonObject object = document.object();
    QByteArray compressed = QByteArray::fromBase64(
        object.value(QStringLiteral("image")).toString().toLatin1());
    const quint32 imageSize = quint32(
        object.value(QStringLiteral("image_size")).toInt());
    QByteArray wrapped(4, '\0');
    qToBigEndian<quint32>(imageSize,
        reinterpret_cast<uchar *>(wrapped.data()));
    wrapped.append(compressed);
    const QByteArray image = qUncompress(wrapped);
    QCOMPARE(image.size(), int(imageSize));
    QCOMPARE(qFromLittleEndian<quint16>(
                 reinterpret_cast<const uchar *>(image.constData() + 28)),
             quint16(8));
    QCOMPARE(image.mid(30, 8), QByteArrayLiteral("A=1\nB=2\n"));
    QVERIFY(view.Log().contains(QStringLiteral("capacity 32")));
    QVERIFY(view.Log().contains(QStringLiteral("both CRC values in 1 unsigned firmware descriptor")));
    QVERIFY(view.Log().contains(QStringLiteral("APJ defaults embedding warning")));

    const QString signedFirmware = dir.filePath(QStringLiteral("signed.apj"));
    QVERIFY(writeFixture(signedFirmware, embeddableApj(true)));
    view.EmbedDefaultsInApj(signedFirmware, parameters);
    QTRY_VERIFY_WITH_TIMEOUT(tool(view, "EmbedDefaultsInApjButton")->isEnabled(),
                             5000);
    QVERIFY(!QFile::exists(signedFirmware + QStringLiteral("new.apj")));
    QVERIFY(view.Log().contains(
        QStringLiteral("Signed or ambiguously marked firmware")));
}

void ConfigDeveloperToolsViewTest::apjEmbeddingCancellationLifetimeAndInterlocks()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString firmware = dir.filePath(QStringLiteral("cancel.apj"));
    const QString parameters = dir.filePath(QStringLiteral("cancel.param"));
    const QString output = firmware + QStringLiteral("new.apj");
    QVERIFY(writeFixture(firmware, embeddableApj()));
    QVERIFY(writeFixture(parameters, QByteArrayLiteral("A=1\n")));
    VehicleFixture fixture;
    DeveloperFtpStub ftp;
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.service);
    view.setMavFtpDownloadServices(&ftp, &fixture.targets);
    view.show();

    {
        PausedGlobalPool paused;
        QVERIFY(paused.ready);
        view.EmbedDefaultsInApj(firmware, parameters);
        auto *progress = view.findChild<QProgressDialog *>(
            QStringLiteral("DeveloperApjProgressDialog"));
        QVERIFY(progress);
        QVERIFY(!tool(view, "ExtractGpsCorrectionsButton")->isEnabled());
        QVERIFY(!tool(view, "SplitDataFlashLogButton")->isEnabled());
        QVERIFY(!tool(view, "CreateDashWareCsvButton")->isEnabled());
        QVERIFY(!tool(view, "OrganizeLogDirectoryButton")->isEnabled());
        QVERIFY(!tool(view, "DownloadMavftpFileButton")->isEnabled());
        QVERIFY(!tool(view, "RebootVehicleButton")->isEnabled());
        view.close();
    }
    QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(output), 5000);
    view.show();
    QTRY_VERIFY_WITH_TIMEOUT(tool(view, "EmbedDefaultsInApjButton")->isEnabled(),
                             5000);

    const QString csv = dir.filePath(QStringLiteral("busy.csv"));
    const QString log = dir.filePath(QStringLiteral("busy.log"));
    QVERIFY(writeFixture(log, dashWareAsciiLog(20)));
    {
        PausedGlobalPool paused;
        QVERIFY(paused.ready);
        view.ExportDashWareCsv(log, csv, {QStringLiteral("GPS")});
        QVERIFY(!tool(view, "EmbedDefaultsInApjButton")->isEnabled());
        view.EmbedDefaultsInApj(firmware, parameters);
        QVERIFY(!view.findChild<QProgressDialog *>(
            QStringLiteral("DeveloperApjProgressDialog")));
        auto *progress = view.findChild<QProgressDialog *>(
            QStringLiteral("DeveloperDashWareProgressDialog"));
        QVERIFY(progress);
        progress->cancel();
    }
    QTRY_VERIFY_WITH_TIMEOUT(tool(view, "EmbedDefaultsInApjButton")->isEnabled(),
                             5000);

    auto *heapView = new ConfigDeveloperToolsView;
    QPointer<ConfigDeveloperToolsView> heapGuard(heapView);
    {
        PausedGlobalPool paused;
        QVERIFY(paused.ready);
        heapView->EmbedDefaultsInApj(firmware, parameters);
        QVERIFY(heapView->findChild<QProgressDialog *>(
            QStringLiteral("DeveloperApjProgressDialog")));
        delete heapView;
        QVERIFY(heapGuard.isNull());
    }
    QVERIFY(!QFile::exists(output));
}

void ConfigDeveloperToolsViewTest::logOrganizerDialogsAreDefaultCancelAndReadOnly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = dir.filePath(QStringLiteral("logs"));
    const QString incoming = QDir(root).filePath(QStringLiteral("new"));
    QVERIFY(QDir().mkpath(incoming));
    const QString source = QDir(incoming).filePath(QStringLiteral("small.log"));
    const QString companion = source + QStringLiteral(".param");
    const QString empty = QDir(incoming).filePath(QStringLiteral("empty.bin"));
    QVERIFY(writeFixture(source, QByteArrayLiteral("recorded bytes")));
    QVERIFY(writeFixture(companion, QByteArrayLiteral("A=1\n")));
    QVERIFY(writeFixture(empty, QByteArray()));

    ConfigDeveloperToolsView view;
    view.show();
    auto *button = tool(view, "OrganizeLogDirectoryButton");
    QVERIFY(button->isEnabled());

    // Cancelling the directory picker never begins analysis.
    button->click();
    auto *picker = visibleNamed<QFileDialog>(
        &view, "DeveloperLogOrganizerDirectoryDialog");
    QVERIFY(picker);
    QCOMPARE(picker->fileMode(), QFileDialog::Directory);
    QVERIFY(picker->testOption(QFileDialog::ShowDirsOnly));
    picker->reject();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(button->isEnabled());
    QVERIFY(QFile::exists(source));
    QVERIFY(QFile::exists(empty));

    button->click();
    picker = visibleNamed<QFileDialog>(
        &view, "DeveloperLogOrganizerDirectoryDialog");
    QVERIFY(picker);
    picker->selectFile(root);
    QVERIFY(QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection));

    QDialog *planDialog = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        (planDialog = visibleNamed<QDialog>(
             &view, "DeveloperLogOrganizerPlanDialog")),
        5000);
    QCOMPARE(planDialog->windowModality(), Qt::NonModal);
    auto *tree = planDialog->findChild<QTreeWidget *>(
        QStringLiteral("DeveloperLogOrganizerPlanTree"));
    QVERIFY(tree);
    QCOMPARE(tree->topLevelItemCount(), 3);
    const QString destination = QDir(root).filePath(
        QStringLiteral("SMALL/small.log"));
    const QString companionDestination = destination + QStringLiteral(".param");
    bool sawMove = false;
    bool sawCompanion = false;
    bool sawDelete = false;
    for (int row = 0; row < tree->topLevelItemCount(); ++row) {
        const QTreeWidgetItem *item = tree->topLevelItem(row);
        if (item->data(1, Qt::UserRole).toString() == source
            && item->data(2, Qt::UserRole).toString() == destination
            && item->text(3) == QString::number(QFileInfo(source).size())) {
            sawMove = true;
        }
        if (item->data(1, Qt::UserRole).toString() == companion
            && item->data(2, Qt::UserRole).toString() == companionDestination) {
            sawCompanion = true;
        }
        if (item->data(1, Qt::UserRole).toString() == empty
            && item->text(2).contains("deleted")
            && item->text(3) == QStringLiteral("0")) {
            sawDelete = true;
        }
    }
    QVERIFY(sawMove);
    QVERIFY(sawCompanion);
    QVERIFY(sawDelete);
    QVERIFY(!tree->header()->stretchLastSection());
    for (int row = 0; row < tree->topLevelItemCount(); ++row) {
        const auto *item = tree->topLevelItem(row);
        QCOMPARE(QDir(root).filePath(item->text(1)),
                 item->data(1, Qt::UserRole).toString());
        QCOMPARE(item->toolTip(1), item->data(1, Qt::UserRole).toString());
    }
    auto *execute = planDialog->findChild<QPushButton *>(
        QStringLiteral("DeveloperLogOrganizerExecuteButton"));
    auto *cancel = planDialog->findChild<QPushButton *>(
        QStringLiteral("DeveloperLogOrganizerCancelButton"));
    QVERIFY(execute && execute->isEnabled());
    QVERIFY(cancel && cancel->isDefault());
    auto *summary = planDialog->findChild<QLabel *>(
        QStringLiteral("DeveloperLogOrganizerPlanSummary"));
    QVERIFY(summary);
    QVERIFY(summary->text().contains(root));
    QVERIFY(summary->text().contains(QStringLiteral("Candidates examined: 2")));
    QVERIFY(summary->text().contains(
        QStringLiteral("permanent"), Qt::CaseInsensitive));

    QTest::keyClick(planDialog, Qt::Key_Escape);
    QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 2000);
    QVERIFY(QFile::exists(source));
    QVERIFY(QFile::exists(companion));
    QVERIFY(QFile::exists(empty));
    QVERIFY(!QFile::exists(destination));
    QVERIFY(view.Log().contains(
        QStringLiteral("no planned changes were executed")));

    const QString emptyRoot = dir.filePath(QStringLiteral("no-candidates"));
    QVERIFY(QDir().mkpath(emptyRoot));
    view.AnalyzeLogDirectory(emptyRoot);
    QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 5000);
    QVERIFY(!visibleNamed<QDialog>(
        &view, "DeveloperLogOrganizerPlanDialog"));
    QVERIFY(view.Log().contains(
        QStringLiteral("No changes were planned")));
}

void ConfigDeveloperToolsViewTest::logOrganizerExecutesExactPlanAndNeverClobbers()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ConfigDeveloperToolsView view;
    view.show();
    auto *organize = tool(view, "OrganizeLogDirectoryButton");

    const QString root = dir.filePath(QStringLiteral("execute"));
    const QString incoming = QDir(root).filePath(QStringLiteral("new"));
    QVERIFY(QDir().mkpath(incoming));
    const QString source = QDir(incoming).filePath(QStringLiteral("small.log"));
    const QString empty = QDir(incoming).filePath(QStringLiteral("empty.bin"));
    const QByteArray sourceBytes("one complete small log");
    QVERIFY(writeFixture(source, sourceBytes));
    QVERIFY(writeFixture(empty, QByteArray()));
    const QString destination = QDir(root).filePath(
        QStringLiteral("SMALL/small.log"));

    view.AnalyzeLogDirectory(root);
    QDialog *planDialog = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        (planDialog = visibleNamed<QDialog>(
             &view, "DeveloperLogOrganizerPlanDialog")),
        5000);
    planDialog->findChild<QPushButton *>(
        QStringLiteral("DeveloperLogOrganizerExecuteButton"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(organize->isEnabled(), 5000);
    QVERIFY(!QFile::exists(source));
    QVERIFY(!QFile::exists(empty));
    QCOMPARE(readFixture(destination), sourceBytes);
    QVERIFY(view.Log().contains(QStringLiteral("Permanently deleted planned")));
    QVERIFY(view.Log().contains(QStringLiteral("Log organization completed")));

    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const QString blockedRoot = dir.filePath(QStringLiteral("no-clobber"));
    const QString blockedIncoming = QDir(blockedRoot).filePath(
        QStringLiteral("new"));
    QVERIFY(QDir().mkpath(blockedIncoming));
    const QString blockedSource = QDir(blockedIncoming).filePath(
        QStringLiteral("blocked.log"));
    const QByteArray blockedBytes("source remains intact");
    QVERIFY(writeFixture(blockedSource, blockedBytes));
    const QString blockedDestination = QDir(blockedRoot).filePath(
        QStringLiteral("SMALL/blocked.log"));

    view.AnalyzeLogDirectory(blockedRoot);
    QTRY_VERIFY_WITH_TIMEOUT(
        (planDialog = visibleNamed<QDialog>(
             &view, "DeveloperLogOrganizerPlanDialog")),
        5000);
    QVERIFY(QDir().mkpath(QFileInfo(blockedDestination).absolutePath()));
    const QByteArray existingBytes("do not replace");
    QVERIFY(writeFixture(blockedDestination, existingBytes));
    planDialog->findChild<QPushButton *>(
        QStringLiteral("DeveloperLogOrganizerExecuteButton"))->click();
    QTRY_VERIFY_WITH_TIMEOUT(organize->isEnabled(), 5000);
    QCOMPARE(readFixture(blockedSource), blockedBytes);
    QCOMPARE(readFixture(blockedDestination), existingBytes);
    QVERIFY(view.Log().contains(
        QStringLiteral("Destination appeared after analysis")));
    QVERIFY(view.Log().contains(QStringLiteral("0 completed changes")));
}

void ConfigDeveloperToolsViewTest::logOrganizerCancellationLifetimeAndInterlocks()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = dir.filePath(QStringLiteral("cancel"));
    const QString incoming = QDir(root).filePath(QStringLiteral("new"));
    QVERIFY(QDir().mkpath(incoming));
    const QString source = QDir(incoming).filePath(QStringLiteral("small.log"));
    const QString destination = QDir(root).filePath(
        QStringLiteral("SMALL/small.log"));
    QVERIFY(writeFixture(source, QByteArrayLiteral("small")));

    VehicleFixture fixture;
    DeveloperFtpStub ftp;
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.service);
    view.setMavFtpDownloadServices(&ftp, &fixture.targets);
    view.show();

    {
        PausedGlobalPool paused;
        QVERIFY(paused.ready);
        view.AnalyzeLogDirectory(root);
        QVERIFY(visibleNamed<QProgressDialog>(
            &view, "DeveloperLogOrganizerProgressDialog"));
        QVERIFY(!tool(view, "ExtractGpsCorrectionsButton")->isEnabled());
        QVERIFY(!tool(view, "SplitDataFlashLogButton")->isEnabled());
        QVERIFY(!tool(view, "CreateDashWareCsvButton")->isEnabled());
        QVERIFY(!tool(view, "EmbedDefaultsInApjButton")->isEnabled());
        QVERIFY(!tool(view, "DownloadMavftpFileButton")->isEnabled());
        QVERIFY(!tool(view, "RebootVehicleButton")->isEnabled());
        view.close();
    }
    QVERIFY(QFile::exists(source));
    QVERIFY(!QFile::exists(destination));
    view.show();
    QTRY_VERIFY_WITH_TIMEOUT(
        tool(view, "OrganizeLogDirectoryButton")->isEnabled(), 5000);

    // Cancellation from the execution progress page happens before the queued
    // immutable plan can make its first filesystem change.
    view.AnalyzeLogDirectory(root);
    QDialog *planDialog = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        (planDialog = visibleNamed<QDialog>(
             &view, "DeveloperLogOrganizerPlanDialog")),
        5000);
    {
        PausedGlobalPool paused;
        QVERIFY(paused.ready);
        planDialog->findChild<QPushButton *>(
            QStringLiteral("DeveloperLogOrganizerExecuteButton"))->click();
        auto *progress = visibleNamed<QProgressDialog>(
            &view, "DeveloperLogOrganizerProgressDialog");
        QVERIFY(progress);
        auto *cancel = progress->findChild<QPushButton *>();
        QVERIFY(cancel);
        cancel->click();
    }
    QTRY_VERIFY_WITH_TIMEOUT(
        tool(view, "OrganizeLogDirectoryButton")->isEnabled(), 5000);
    QVERIFY(QFile::exists(source));
    QVERIFY(!QFile::exists(destination));
    QVERIFY(view.Log().contains(
        QStringLiteral("Log organization cancelled")));

    auto *heapView = new ConfigDeveloperToolsView;
    QPointer<ConfigDeveloperToolsView> heapGuard(heapView);
    {
        PausedGlobalPool paused;
        QVERIFY(paused.ready);
        heapView->AnalyzeLogDirectory(root);
        QVERIFY(visibleNamed<QProgressDialog>(
            heapView, "DeveloperLogOrganizerProgressDialog"));
        delete heapView;
        QVERIFY(heapGuard.isNull());
    }
    QVERIFY(QFile::exists(source));
    QVERIFY(!QFile::exists(destination));
}

void ConfigDeveloperToolsViewTest::parameterRecoveryBindingAndDefaultCancel()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString parameters = dir.filePath(
        QStringLiteral("%4 recovery.param"));
    QVERIFY(writeFixture(parameters,
        QByteArrayLiteral("TEST_ENABLE,1\nDEVICE_ID,202\nGAIN,12.5\n")));

    VehicleFixture fixture;
    ParameterRecoveryService recovery(
        &fixture.targets, &fixture.registry, &fixture.parameters,
        &fixture.commands,
        [](const SwarmVehicleInstanceLease &, QString *) { return true; });
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.service);
    QCOMPARE(view.ImplementedActionCount(), 17);
    QVERIFY(!tool(view, "RestoreParametersButton")->isEnabled());
    QVERIFY(!tool(view, "CancelParameterRestoreButton")->isEnabled());

    view.setParameterRecoveryService(&recovery);
    QCOMPARE(view.ImplementedActionCount(), 19);
    view.show();
    QTRY_VERIFY(tool(view, "RestoreParametersButton")->isEnabled());
    QVERIFY(!tool(view, "CancelParameterRestoreButton")->isEnabled());

    tool(view, "RestoreParametersButton")->click();
    auto *picker = visibleNamed<QFileDialog>(
        &view, "DeveloperParameterRecoveryFileDialog");
    QVERIFY(picker);
    const QString filters = picker->nameFilters().join(QLatin1Char(';'));
    QVERIFY(filters.contains(QStringLiteral("*.param")));
    QVERIFY(filters.contains(QStringLiteral("*.parm")));
    picker->reject();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(fixture.frames.isEmpty());
    QTRY_VERIFY(tool(view, "RestoreParametersButton")->isEnabled());

    tool(view, "RestoreParametersButton")->click();
    picker = visibleNamed<QFileDialog>(
        &view, "DeveloperParameterRecoveryFileDialog");
    QVERIFY(picker);
    picker->selectFile(parameters);
    QVERIFY(QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection));
    auto *confirm = visibleNamed<QMessageBox>(
        &view, "DeveloperParameterRecoveryConfirmation");
    QVERIFY(confirm);
    QCOMPARE(confirm->defaultButton(), confirm->button(QMessageBox::Cancel));
    QCOMPARE(confirm->escapeButton(), confirm->button(QMessageBox::Cancel));
    QVERIFY(confirm->text().contains(parameters));
    QVERIFY(confirm->text().contains(QStringLiteral("Bench vehicle")));
    QVERIFY(confirm->text().contains(QStringLiteral("link 7, system 42, component 1")));
    QVERIFY(confirm->text().contains(
        QStringLiteral("ENABLE parameters are applied")));
    QVERIFY(confirm->text().contains(QStringLiteral("*_ID")));
    QVERIFY(confirm->text().contains(QStringLiteral("reset to zero")));
    QVERIFY(confirm->text().contains(QStringLiteral("not rolled back")));
    confirm->button(QMessageBox::Cancel)->click();
    QVERIFY(fixture.frames.isEmpty());
    QVERIFY(!recovery.busy());

    ParameterRecoveryService::Plan foreignPlan;
    QString error;
    QVERIFY(recovery.prepare(parameters, &foreignPlan, &error));
    quint64 foreignOperation = 0;
    QCOMPARE(recovery.execute(foreignPlan, &foreignOperation, &error),
             ParameterRecoveryService::SubmitResult::Started);
    QVERIFY(foreignOperation != 0);
    QVERIFY(recovery.busy());
    QVERIFY(!tool(view, "CancelParameterRestoreButton")->isEnabled());
    tool(view, "CancelParameterRestoreButton")->click();
    QVERIFY(recovery.busy());
    QVERIFY(recovery.cancel(foreignOperation));
    QTRY_VERIFY_WITH_TIMEOUT(!recovery.busy(), 2000);

    view.setParameterRecoveryService(nullptr);
    QCOMPARE(view.ImplementedActionCount(), 17);
    QVERIFY(!tool(view, "RestoreParametersButton")->isEnabled());
}

void ConfigDeveloperToolsViewTest::parameterRecoveryRejectsChangedOrArmedTarget()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString parameters = dir.filePath(QStringLiteral("stale.param"));
    QVERIFY(writeFixture(parameters, QByteArrayLiteral("GAIN,12.5\n")));
    VehicleFixture fixture;
    std::function<void()> recoveryHook;
    ParameterRecoveryService recovery(
        &fixture.targets, &fixture.registry, &fixture.parameters,
        &fixture.commands,
        [&recoveryHook](const SwarmVehicleInstanceLease &, QString *) {
            const auto hook = recoveryHook;
            if (hook)
                hook();
            return true;
        });
    ConfigDeveloperToolsView view;
    view.setParameterRecoveryService(&recovery);
    view.show();

    const auto openConfirmation = [&]() -> QMessageBox * {
        tool(view, "RestoreParametersButton")->click();
        auto *picker = visibleNamed<QFileDialog>(
            &view, "DeveloperParameterRecoveryFileDialog");
        if (!picker)
            return nullptr;
        picker->selectFile(parameters);
        if (!QMetaObject::invokeMethod(
                picker, "accept", Qt::DirectConnection)) {
            return nullptr;
        }
        return visibleNamed<QMessageBox>(
            &view, "DeveloperParameterRecoveryConfirmation");
    };

    auto *confirm = openConfirmation();
    QVERIFY(confirm);
    VehicleEndpoint other = fixture.endpoint;
    other.linkId = 8;
    QVERIFY(fixture.targets.observeEndpoint(other));
    QVERIFY(fixture.targets.selectTarget(8, 42, 1));
    QVERIFY(fixture.targets.selectTarget(7, 42, 1));
    fixture.heartbeat(false);
    confirm->button(QMessageBox::Yes)->click();
    QVERIFY(fixture.frames.isEmpty());
    QVERIFY(!recovery.busy());
    QVERIFY(view.Log().contains(QStringLiteral("cancelled")));

    QTRY_VERIFY(tool(view, "RestoreParametersButton")->isEnabled());
    confirm = openConfirmation();
    QVERIFY(confirm);
    fixture.heartbeat(true);
    confirm->button(QMessageBox::Yes)->click();
    QVERIFY(fixture.frames.isEmpty());
    QVERIFY(!recovery.busy());
    fixture.heartbeat(false);

    // The service publishes its exact operation token before the final route
    // validator. Closing from that callback cancels this operation and cannot
    // leave a write running without page ownership.
    QTRY_VERIFY(tool(view, "RestoreParametersButton")->isEnabled());
    int admittedRouteCalls = 0;
    recoveryHook = [&view, &recovery, &admittedRouteCalls]() {
        // canPrepare(), prepare() and both UI validations legitimately invoke
        // the same policy before admission.  Close only after execute() has
        // published the owned operation token and busy state.
        if (recovery.busy() && ++admittedRouteCalls == 1)
            view.close();
    };
    confirm = openConfirmation();
    QVERIFY(confirm);
    confirm->button(QMessageBox::Yes)->click();
    QCOMPARE(admittedRouteCalls, 1);
    QVERIFY(fixture.frames.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!recovery.busy(), 2000);
    QCOMPARE(recovery.lastReport().outcome,
             ParameterRecoveryService::Outcome::Cancelled);
    view.show();
    QVERIFY(view.Log().contains(QStringLiteral("cancelled")));
}

void ConfigDeveloperToolsViewTest::parameterRecoveryExecutesOrderedPlanAndReports()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString parameters = dir.filePath(QStringLiteral("ordered.parm"));
    QVERIFY(writeFixture(parameters,
        QByteArrayLiteral("TEST_ENABLE,1\nGAIN,12.5\nDEVICE_ID,202\nSAME,3\n")));
    VehicleFixture fixture;
    ParameterRecoveryService recovery(
        &fixture.targets, &fixture.registry, &fixture.parameters,
        &fixture.commands,
        [](const SwarmVehicleInstanceLease &, QString *) { return true; });
    DeveloperFtpStub ftp;
    ConfigDeveloperToolsView view;
    view.setVehicleToolService(&fixture.service);
    view.setMavFtpDownloadServices(&ftp, &fixture.targets);
    view.setParameterRecoveryService(&recovery);
    QCOMPARE(view.ImplementedActionCount(), 20);
    view.show();

    ftp.setSharedBusy(true);
    QTRY_VERIFY(!tool(view, "RestoreParametersButton")->isEnabled());
    ftp.setSharedBusy(false);
    QTRY_VERIFY(tool(view, "RestoreParametersButton")->isEnabled());

    tool(view, "RestoreParametersButton")->click();
    auto *picker = visibleNamed<QFileDialog>(
        &view, "DeveloperParameterRecoveryFileDialog");
    QVERIFY(picker);
    picker->selectFile(parameters);
    QVERIFY(QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection));
    auto *confirm = visibleNamed<QMessageBox>(
        &view, "DeveloperParameterRecoveryConfirmation");
    QVERIFY(confirm);
    confirm->button(QMessageBox::Yes)->click();
    QVERIFY(recovery.busy());
    QVERIFY(visibleNamed<QProgressDialog>(
        &view, "DeveloperParameterRecoveryProgressDialog"));
    QVERIFY(tool(view, "CancelParameterRestoreButton")->isEnabled());
    QVERIFY(!tool(view, "ExtractGpsCorrectionsButton")->isEnabled());
    QVERIFY(!tool(view, "DownloadMavftpFileButton")->isEnabled());
    QVERIFY(!tool(view, "RebootVehicleButton")->isEnabled());

    QMap<QString, float> current{
        {QStringLiteral("TEST_ENABLE"), 0.0f},
        {QStringLiteral("GAIN"), 1.0f},
        {QStringLiteral("DEVICE_ID"), 100.0f},
        {QStringLiteral("SAME"), 3.0f}};
    const QMap<QString, int> indexes{
        {QStringLiteral("TEST_ENABLE"), 0},
        {QStringLiteral("GAIN"), 1},
        {QStringLiteral("DEVICE_ID"), 2},
        {QStringLiteral("SAME"), 3}};
    QStringList writes;
    QList<float> writeValues;
    int handled = 0;
    int guard = 0;
    while (recovery.busy() && guard++ < 40) {
        // The queued exact terminal report for the preceding reply can finish
        // the recovery without producing another frame.  Wait for either
        // outcome instead of requiring a nonexistent post-terminal request.
        QTRY_VERIFY_WITH_TIMEOUT(!recovery.busy()
                                 || fixture.frames.size() > handled, 2000);
        if (!recovery.busy())
            break;
        const mavlink_message_t message = fixture.messageAt(handled++);
        if (message.msgid == MAVLINK_MSG_ID_PARAM_REQUEST_READ) {
            mavlink_param_request_read_t request{};
            mavlink_msg_param_request_read_decode(&message, &request);
            const QString name = mavlinkParameterName(
                request.param_id, sizeof request.param_id);
            QVERIFY2(current.contains(name), qPrintable(name));
            fixture.parameterReply(name, current.value(name),
                                   MAV_PARAM_TYPE_REAL32,
                                   indexes.value(name));
        } else if (message.msgid == MAVLINK_MSG_ID_PARAM_SET) {
            mavlink_param_set_t request{};
            mavlink_msg_param_set_decode(&message, &request);
            const QString name = mavlinkParameterName(
                request.param_id, sizeof request.param_id);
            QVERIFY2(current.contains(name), qPrintable(name));
            writes.append(name);
            writeValues.append(request.param_value);
            current.insert(name, request.param_value);
            fixture.parameterReply(name, request.param_value,
                                   static_cast<MAV_PARAM_TYPE>(request.param_type),
                                   indexes.value(name));
        } else {
            QFAIL("Unexpected frame during parameter recovery");
        }
        QCoreApplication::processEvents();
    }
    QVERIFY2(!recovery.busy(), "parameter recovery did not reach a terminal report");
    QCOMPARE(writes, QStringList({QStringLiteral("TEST_ENABLE"),
                                  QStringLiteral("GAIN"),
                                  QStringLiteral("DEVICE_ID"),
                                  QStringLiteral("DEVICE_ID")}));
    QCOMPARE(writeValues.size(), 4);
    QCOMPARE(writeValues.at(0), 1.0f);
    QCOMPARE(writeValues.at(1), 12.5f);
    QCOMPARE(writeValues.at(2), 0.0f);
    QCOMPARE(writeValues.at(3), 202.0f);
    const auto report = recovery.lastReport();
    QCOMPARE(report.outcome, ParameterRecoveryService::Outcome::Completed);
    QCOMPARE(report.setCount, 2);
    QCOMPARE(report.unchangedCount, 2);
    QCOMPARE(report.failedCount, 0);
    int enableReceipts = 0;
    int resetReceipts = 0;
    for (const auto &receipt : report.receipts) {
        enableReceipts += receipt.kind
            == ParameterRecoveryService::Receipt::Kind::EnableWrite;
        resetReceipts += receipt.kind
            == ParameterRecoveryService::Receipt::Kind::IdentifierReset;
    }
    QCOMPARE(enableReceipts, 1);
    QCOMPARE(resetReceipts, 1);
    QVERIFY(view.Log().contains(QStringLiteral("2 set, 2 unchanged, 0 failed")));
    QVERIFY(view.Log().contains(QStringLiteral("1 identifier resets to zero")));
    QVERIFY(!tool(view, "CancelParameterRestoreButton")->isEnabled());
    QTRY_VERIFY(tool(view, "RestoreParametersButton")->isEnabled());
}

void ConfigDeveloperToolsViewTest::parameterRecoveryCancellationIsOwnedAndRetained()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString parameters = dir.filePath(QStringLiteral("partial.param"));
    QVERIFY(writeFixture(parameters,
        QByteArrayLiteral("TEST_ENABLE,1\nGAIN,12.5\n")));
    VehicleFixture fixture;
    ParameterRecoveryService recovery(
        &fixture.targets, &fixture.registry, &fixture.parameters,
        &fixture.commands,
        [](const SwarmVehicleInstanceLease &, QString *) { return true; });
    ConfigDeveloperToolsView view;
    view.setParameterRecoveryService(&recovery);
    view.show();
    tool(view, "RestoreParametersButton")->click();
    auto *picker = visibleNamed<QFileDialog>(
        &view, "DeveloperParameterRecoveryFileDialog");
    QVERIFY(picker);
    picker->selectFile(parameters);
    QVERIFY(QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection));
    auto *confirm = visibleNamed<QMessageBox>(
        &view, "DeveloperParameterRecoveryConfirmation");
    QVERIFY(confirm);
    confirm->button(QMessageBox::Yes)->click();

    QMap<QString, float> current{
        {QStringLiteral("TEST_ENABLE"), 0.0f},
        {QStringLiteral("GAIN"), 1.0f}};
    int handled = 0;
    bool acknowledgedEnable = false;
    while (recovery.busy() && !acknowledgedEnable) {
        QTRY_VERIFY_WITH_TIMEOUT(fixture.frames.size() > handled, 2000);
        const mavlink_message_t message = fixture.messageAt(handled++);
        if (message.msgid == MAVLINK_MSG_ID_PARAM_REQUEST_READ) {
            mavlink_param_request_read_t request{};
            mavlink_msg_param_request_read_decode(&message, &request);
            const QString name = mavlinkParameterName(
                request.param_id, sizeof request.param_id);
            fixture.parameterReply(name, current.value(name),
                                   MAV_PARAM_TYPE_REAL32,
                                   name == QStringLiteral("TEST_ENABLE") ? 0 : 1,
                                   2);
        } else {
            mavlink_param_set_t request{};
            mavlink_msg_param_set_decode(&message, &request);
            const QString name = mavlinkParameterName(
                request.param_id, sizeof request.param_id);
            QCOMPARE(name, QStringLiteral("TEST_ENABLE"));
            current.insert(name, request.param_value);
            fixture.parameterReply(name, request.param_value,
                                   MAV_PARAM_TYPE_REAL32, 0, 2);
            acknowledgedEnable = true;
        }
    }
    QVERIFY(acknowledgedEnable);
    QVERIFY(recovery.busy());
    view.close(); // Cancels exactly this page's token, not a foreign operation.
    QTRY_VERIFY_WITH_TIMEOUT(!recovery.busy(), 2000);
    QCOMPARE(recovery.lastReport().outcome,
             ParameterRecoveryService::Outcome::Cancelled);
    QCOMPARE(recovery.lastReport().receipts.size(), 1);
    QCOMPARE(recovery.lastReport().receipts.first().kind,
             ParameterRecoveryService::Receipt::Kind::EnableWrite);
    view.show();
    QTRY_VERIFY_WITH_TIMEOUT(
        view.Log().contains(QStringLiteral("possibly partial")), 2000);
    QVERIFY(view.Log().contains(QStringLiteral("1 ENABLE")));
    QVERIFY(view.Log().contains(QStringLiteral("not roll back")));

    ConfigDeveloperToolsView reopened;
    reopened.setParameterRecoveryService(&recovery);
    QVERIFY(reopened.Log().contains(recovery.lastReport().description));
}

void ConfigDeveloperToolsViewTest::remoteDataFlashBindingAndStartConsent()
{
    RemoteDataFlashFixture fixture;
    QVERIFY(fixture.ready);
    ConfigDeveloperToolsView view;
    view.setRemoteDataFlashLogService(
        &fixture.service, fixture.directory.path());
    QCOMPARE(view.ImplementedActionCount(), 12);
    view.show();

    auto *start = tool(view, "StartRemoteDataFlashLogButton");
    auto *stop = tool(view, "StopRemoteDataFlashLogButton");
    QVERIFY(start);
    QVERIFY(stop);
    QTRY_VERIFY(start->isEnabled());
    QVERIFY(!stop->isEnabled());
    start->click();
    auto *confirm = visibleNamed<QMessageBox>(
        &view, "DeveloperRemoteDataFlashStartConfirmation");
    QVERIFY(confirm);
    QCOMPARE(confirm->textFormat(), Qt::PlainText);
    QCOMPARE(confirm->defaultButton(),
             confirm->button(QMessageBox::Cancel));
    QCOMPARE(confirm->escapeButton(),
             confirm->button(QMessageBox::Cancel));
    QVERIFY(confirm->text().contains(fixture.vehicle.endpoint.displayName()));
    QVERIFY(confirm->text().contains(fixture.directory.path()));
    QVERIFY(confirm->text().contains(QStringLiteral("LOG_BACKEND_TYPE")));
    QVERIFY(confirm->text().contains(QStringLiteral("dedicated trusted")));
    QVERIFY(confirm->text().contains(QStringLiteral("no START acknowledgement")));
    QVERIFY(!tool(view, "ExtractGpsCorrectionsButton")->isEnabled());
    QVERIFY(!tool(view, "RebootVehicleButton")->isEnabled());
    QVERIFY(fixture.vehicle.frames.isEmpty());

    confirm->button(QMessageBox::Cancel)->click();
    QTRY_VERIFY(start->isEnabled());
    QVERIFY(!fixture.service.busy());
    QVERIFY(fixture.vehicle.frames.isEmpty());

    start->click();
    QVERIFY(visibleNamed<QMessageBox>(
        &view, "DeveloperRemoteDataFlashStartConfirmation"));
    view.close();
    QVERIFY(!fixture.service.busy());
    QVERIFY(fixture.vehicle.frames.isEmpty());
    view.show();
    QTRY_VERIFY(start->isEnabled());

    view.setRemoteDataFlashLogService(nullptr, QString());
    QCOMPARE(view.ImplementedActionCount(), 10);
    QVERIFY(!start->isEnabled());
    QVERIFY(!stop->isEnabled());

    auto *disposable = new RemoteDataFlashLogService(
        &fixture.vehicle.targets, &fixture.vehicle.registry,
        &fixture.vehicle.parameters, &fixture.vehicle.transmitter,
        250, 190,
        [](const SwarmVehicleInstanceLease &, QString *) { return true; });
    view.setRemoteDataFlashLogService(
        disposable, fixture.directory.path());
    QTRY_VERIFY(start->isEnabled());
    start->click();
    QVERIFY(visibleNamed<QMessageBox>(
        &view, "DeveloperRemoteDataFlashStartConfirmation"));
    delete disposable;
    QTRY_VERIFY(!start->isEnabled());
    QVERIFY(!visibleNamed<QMessageBox>(
        &view, "DeveloperRemoteDataFlashStartConfirmation"));
}

void ConfigDeveloperToolsViewTest::remoteDataFlashSessionSurvivesCloseAndSaves()
{
    RemoteDataFlashFixture fixture;
    QVERIFY(fixture.ready);
    auto *first = new ConfigDeveloperToolsView;
    first->setRemoteDataFlashLogService(
        &fixture.service, fixture.directory.path());
    first->show();
    QTRY_VERIFY(tool(*first, "StartRemoteDataFlashLogButton")->isEnabled());
    tool(*first, "StartRemoteDataFlashLogButton")->click();
    auto *startConfirm = visibleNamed<QMessageBox>(
        first, "DeveloperRemoteDataFlashStartConfirmation");
    QVERIFY(startConfirm);
    startConfirm->button(QMessageBox::Yes)->click();
    QVERIFY(fixture.service.busy());
    // Writer completion is posted back to the GUI thread, so Opening remains
    // observable until the event loop is allowed to deliver that callback.
    QCOMPARE(fixture.service.phase(),
             RemoteDataFlashLogService::Phase::Opening);
    auto *openingStop = tool(*first, "StopRemoteDataFlashLogButton");
    QVERIFY(!openingStop->isEnabled());
    QVERIFY(openingStop->toolTip().contains(QStringLiteral("still opening")));

    // Even a programmatic enable must not bypass the service phase gate.
    openingStop->setEnabled(true);
    openingStop->click();
    QVERIFY(!visibleNamed<QMessageBox>(
        first, "DeveloperRemoteDataFlashStopConfirmation"));
    QCOMPARE(fixture.service.phase(),
             RemoteDataFlashLogService::Phase::Opening);
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.vehicle.frames.isEmpty(), 2000);
    QVERIFY(visibleNamed<QProgressDialog>(
        first, "DeveloperRemoteDataFlashProgressDialog"));

    fixture.block(0, 'L');
    QTRY_COMPARE_WITH_TIMEOUT(fixture.service.blocksStored(), qint64(1), 2000);
    const quint64 operationId = fixture.service.currentOperationId();
    QVERIFY(operationId != 0);
    first->close();
    delete first;
    QVERIFY(fixture.service.busy());
    QCOMPARE(fixture.service.currentOperationId(), operationId);

    ConfigDeveloperToolsView reopened;
    reopened.setRemoteDataFlashLogService(
        &fixture.service, fixture.directory.path());
    reopened.show();
    auto *stop = tool(reopened, "StopRemoteDataFlashLogButton");
    QTRY_VERIFY(stop->isEnabled());
    QVERIFY(visibleNamed<QProgressDialog>(
        &reopened, "DeveloperRemoteDataFlashProgressDialog"));
    stop->click();
    auto *stopConfirm = visibleNamed<QMessageBox>(
        &reopened, "DeveloperRemoteDataFlashStopConfirmation");
    QVERIFY(stopConfirm);
    QCOMPARE(stopConfirm->defaultButton(),
             stopConfirm->button(QMessageBox::Cancel));
    QCOMPARE(stopConfirm->escapeButton(),
             stopConfirm->button(QMessageBox::Cancel));
    QVERIFY(stopConfirm->text().contains(
        fixture.vehicle.endpoint.displayName()));
    QVERIFY(stopConfirm->text().contains(QStringLiteral("1 blocks")));
    QVERIFY(stopConfirm->text().contains(QStringLiteral("no STOP acknowledgement")));
    stopConfirm->button(QMessageBox::Cancel)->click();
    QVERIFY(fixture.service.busy());

    QTRY_VERIFY(stop->isEnabled());
    stop->click();
    stopConfirm = visibleNamed<QMessageBox>(
        &reopened, "DeveloperRemoteDataFlashStopConfirmation");
    QVERIFY(stopConfirm);
    stopConfirm->button(QMessageBox::Save)->click();
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.service.busy(), 5000);
    const RemoteDataFlashLogService::Report report =
        fixture.service.lastReport();
    QCOMPARE(report.operationId, operationId);
    QCOMPARE(report.outcome,
             RemoteDataFlashLogService::Outcome::SavedUnverified);
    QCOMPARE(report.blocks, qint64(1));
    QCOMPARE(report.bytes, qint64(200));
    QVERIFY(report.published());
    QVERIFY(QFileInfo::exists(report.destinationPath));
    QCOMPARE(readFixture(report.destinationPath), QByteArray(200, 'L'));
    QTRY_VERIFY(reopened.Log().contains(QStringLiteral("not proof")));
}

void ConfigDeveloperToolsViewTest::remoteDataFlashRejectsStaleConsentAndOldStopToken()
{
    RemoteDataFlashFixture fixture;
    QVERIFY(fixture.ready);
    ConfigDeveloperToolsView view;
    view.setRemoteDataFlashLogService(
        &fixture.service, fixture.directory.path());
    view.show();
    auto *start = tool(view, "StartRemoteDataFlashLogButton");
    QTRY_VERIFY(start->isEnabled());
    start->click();
    auto *confirm = visibleNamed<QMessageBox>(
        &view, "DeveloperRemoteDataFlashStartConfirmation");
    QVERIFY(confirm);
    fixture.vehicle.targets.clearTarget();
    QVERIFY(fixture.vehicle.targets.selectTarget(
        fixture.vehicle.endpoint.linkId,
        fixture.vehicle.endpoint.systemId,
        fixture.vehicle.endpoint.componentId));
    fixture.vehicle.heartbeat(false);
    confirm->button(QMessageBox::Yes)->click();
    QTRY_VERIFY(!fixture.service.busy());
    QVERIFY(fixture.vehicle.frames.isEmpty());
    QVERIFY(view.Log().contains(QStringLiteral("cancelled before start")));

    QTRY_VERIFY(start->isEnabled());
    start->click();
    confirm = visibleNamed<QMessageBox>(
        &view, "DeveloperRemoteDataFlashStartConfirmation");
    QVERIFY(confirm);
    confirm->button(QMessageBox::Yes)->click();
    QTRY_VERIFY_WITH_TIMEOUT(fixture.service.busy(), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.vehicle.frames.isEmpty(), 2000);
    fixture.block(0, 'A');
    QTRY_COMPARE_WITH_TIMEOUT(fixture.service.blocksStored(), qint64(1), 2000);
    const quint64 oldOperation = fixture.service.currentOperationId();
    tool(view, "StopRemoteDataFlashLogButton")->click();
    auto *oldStop = visibleNamed<QMessageBox>(
        &view, "DeveloperRemoteDataFlashStopConfirmation");
    QVERIFY(oldStop);

    QString error;
    QVERIFY(fixture.service.cancel(oldOperation, &error));
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.service.busy(), 3000);
    RemoteDataFlashLogService::Plan replacement;
    QVERIFY2(fixture.service.prepare(fixture.directory.path(),
                                     &replacement, &error),
             qPrintable(error));
    quint64 replacementOperation = 0;
    QCOMPARE(fixture.service.start(replacement, &replacementOperation, &error),
             RemoteDataFlashLogService::StartResult::Started);
    QVERIFY(replacementOperation != 0);
    QVERIFY(replacementOperation != oldOperation);
    QVERIFY(fixture.service.busy());

    oldStop->button(QMessageBox::Save)->click();
    QVERIFY(fixture.service.busy());
    QCOMPARE(fixture.service.currentOperationId(), replacementOperation);
    QVERIFY(view.Log().contains(QStringLiteral("session changed")));
    QVERIFY(fixture.service.cancel(replacementOperation, &error));
    QTRY_VERIFY_WITH_TIMEOUT(!fixture.service.busy(), 3000);
}

QTEST_MAIN(ConfigDeveloperToolsViewTest)

#include "test_configdevelopertoolsview.moc"
